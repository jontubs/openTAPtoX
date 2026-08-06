#!/usr/bin/env python3
"""Build a compact, reproducible index over all historical TAP captures."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter, defaultdict
from dataclasses import asdict
from pathlib import Path
from typing import Any, Dict, Iterable, List

from tigo_tap_decoder_v2 import (
    ParseDiagnostics,
    decode_active_command,
    decode_node_table_response,
    parse_rx_status_and_blob,
    read_gateway_frames,
    split_pv_packets,
)


CAPTURE_SUFFIXES = {".log", ".jsonl", ".txt", ".zip", ".gz"}
MIRROR_SUFFIXES = (
    ".raw_mqtt",
    ".normal_counter",
    ".interesting",
    ".frames",
    ".events",
    ".status",
    ".summary",
    ".analysis",
    ".raw",
    ".meta",
)


def session_key(path: Path) -> str:
    name = path.name
    changed = True
    while changed:
        changed = False
        for suffix in (".jsonl.zip", ".log.gz", ".jsonl", ".log", ".txt", ".zip", ".gz"):
            if name.endswith(suffix):
                name = name[: -len(suffix)]
                changed = True
                break
        for suffix in MIRROR_SUFFIXES:
            if name.endswith(suffix):
                name = name[: -len(suffix)]
                changed = True
                break
    parent = path.parent.as_posix()
    return f"{parent}/{name}" if parent not in ("", ".") else name


def iter_capture_paths(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix.lower() in CAPTURE_SUFFIXES:
            yield path


def summarize_capture(path: Path, root: Path) -> Dict[str, Any]:
    diagnostics = ParseDiagnostics()
    frames = read_gateway_frames(path, diagnostics)
    valid_frames = [frame for frame in frames if frame.crc_ok]
    frame_types: Counter[str] = Counter()
    management_types: Counter[str] = Counter()
    active_subcommands: Counter[str] = Counter()
    queue_credits: Counter[str] = Counter()
    packet_types: Counter[str] = Counter()
    tap_euis = set()
    status_channels = set()
    profile_reads: Dict[str, Dict[str, Any]] = {}
    profile_writes: Dict[str, Dict[str, Any]] = {}
    join_seeds = set()
    power_nodes = set()
    topology_nodes = set()
    valid_power_packet_count = 0
    invalid_power_packet_count = 0
    table_snapshots: List[Dict[str, Any]] = []
    network_snapshots: List[Dict[str, Any]] = []
    learn_controls: List[Dict[str, Any]] = []

    def remember_table(table: Dict[str, Any], timestamp: str, source: str) -> None:
        snapshot = {
            "timestamp": timestamp,
            "source": source,
            "start_index": table["start_index"],
            "entry_count": table["entry_count"],
            "pending_count": sum(entry["pending"] for entry in table["entries"]),
            "confirmed_count": sum(not entry["pending"] for entry in table["entries"]),
            "raw_node_ids": [f"0x{entry['node_id_raw']:04X}" for entry in table["entries"]],
        }
        state = {key: value for key, value in snapshot.items() if key not in ("timestamp", "source")}
        if table_snapshots:
            previous = {
                key: value
                for key, value in table_snapshots[-1].items()
                if key not in ("timestamp", "source")
            }
            if state == previous:
                return
        table_snapshots.append(snapshot)

    for frame in valid_frames:
        type_key = f"0x{frame.type_code:04X}"
        frame_types[type_key] += 1
        if frame.type_code not in (0x0148, 0x0149, 0x0B0F, 0x0B10):
            management_types[type_key] += 1

        if frame.from_gateway and frame.type_code in (0x0039, 0x003B) and len(frame.payload) >= 10:
            tap_euis.add(frame.payload[:8].hex().upper())
        if frame.from_gateway and frame.type_code == 0x000F and len(frame.payload) >= 2:
            if frame.payload[0] == 0x20:
                status_channels.add(frame.payload[1])

        if frame.type_code == 0x0149:
            parsed = parse_rx_status_and_blob(frame.payload)
            if parsed is not None:
                _, blob = parsed
                for packet in split_pv_packets(blob):
                    packet_types[f"0x{packet['ptype']:02X}"] += 1
                    if packet["ptype"] == 0x31:
                        if len(packet["data"]) == 13:
                            valid_power_packet_count += 1
                            power_nodes.add(packet["node_id"] & 0x7FFF)
                        else:
                            invalid_power_packet_count += 1
                    elif packet["ptype"] == 0x09:
                        topology_nodes.add(packet["node_id"] & 0x7FFF)
                    elif packet["ptype"] == 0x27:
                        table = decode_node_table_response(packet["data"])
                        if table is not None:
                            remember_table(table, frame.timestamp, "rx_packet")

        active = decode_active_command(frame)
        if active is None:
            continue
        active_subcommands[f"0x{active.subcmd:02X}:{active.direction}"] += 1
        if active.direction == "response" and "tx_buffers_free" in active.decoded:
            queue_credits[f"0x{active.subcmd:02X}:0x{active.decoded['tx_buffers_free']:02X}"] += 1
        descriptor = active.decoded.get("radio_descriptor")
        if descriptor:
            profile_reads[descriptor["fingerprint_sha256_12"]] = descriptor
        descriptor = active.decoded.get("radio_descriptor_write")
        if descriptor:
            profile_writes[descriptor["fingerprint_sha256_12"]] = descriptor
        table = active.decoded.get("node_table")
        if table:
            remember_table(table, frame.timestamp, "active_response")
        network = active.decoded.get("network_status")
        if network:
            network_snapshots.append(network)
        learn = active.decoded.get("learn_control")
        if learn:
            learn_controls.append(learn)
        routed = active.decoded.get("routed_network_control")
        if routed and active.direction == "request" and routed.get("target_u16") == 1:
            join_seeds.add(active.raw_body_hex)

    return {
        "path": str(path.relative_to(root)).replace("\\", "/"),
        "session_key": session_key(path.relative_to(root)),
        "size_bytes": path.stat().st_size,
        "decoded_frames": len(frames),
        "valid_frames": len(valid_frames),
        "crc_bad_frames": len(frames) - len(valid_frames),
        "parse_warning_count": diagnostics.warning_count,
        "parse_warnings": asdict(diagnostics),
        "first_timestamp": valid_frames[0].timestamp if valid_frames else None,
        "last_timestamp": valid_frames[-1].timestamp if valid_frames else None,
        "frame_type_counts": dict(sorted(frame_types.items())),
        "management_type_counts": dict(sorted(management_types.items())),
        "active_subcommand_counts": dict(sorted(active_subcommands.items())),
        "queue_credit_counts": dict(sorted(queue_credits.items())),
        "pv_packet_counts": dict(sorted(packet_types.items())),
        "tap_euis": sorted(tap_euis),
        "status_channels": sorted(status_channels),
        "radio_profile_reads": list(profile_reads.values()),
        "radio_profile_writes": list(profile_writes.values()),
        "generic_join_seeds": sorted(join_seeds),
        "last_node_table": table_snapshots[-1] if table_snapshots else None,
        "node_table_state_changes": table_snapshots,
        "last_network_status": network_snapshots[-1] if network_snapshots else None,
        "learn_controls": learn_controls,
        "power_packet_count": valid_power_packet_count,
        "invalid_power_packet_count": invalid_power_packet_count,
        "power_nodes": sorted(power_nodes),
        "topology_nodes": sorted(topology_nodes),
    }


def build_index(root: Path) -> Dict[str, Any]:
    files: List[Dict[str, Any]] = []
    errors: List[Dict[str, str]] = []
    for path in iter_capture_paths(root):
        try:
            files.append(summarize_capture(path, root))
        except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
            errors.append({"path": str(path.relative_to(root)), "error": str(exc)})

    grouped: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for item in files:
        grouped[item["session_key"]].append(item)
    sessions = []
    for key, mirrors in sorted(grouped.items()):
        decodable_mirrors = [item for item in mirrors if item["valid_frames"] > 0]
        if not decodable_mirrors:
            continue
        richest = max(decodable_mirrors, key=lambda item: (item["valid_frames"], item["size_bytes"]))
        session = dict(richest)
        session["session_key"] = key
        session["mirror_files"] = sorted(item["path"] for item in mirrors)
        sessions.append(session)

    valid_frame_files = [item for item in files if item["valid_frames"] > 0]
    warning_files = [item for item in files if item["parse_warning_count"] > 0]
    power_sessions = [item for item in sessions if item["power_packet_count"] > 0]
    return {
        "root": str(root.resolve()),
        "file_count": len(files),
        "artifact_group_count": len(grouped),
        "session_count": len(sessions),
        "files_with_valid_frames": len(valid_frame_files),
        "empty_or_unsupported_files": len(files) - len(valid_frame_files),
        "files_with_parse_warnings": len(warning_files),
        "parse_warning_count": sum(item["parse_warning_count"] for item in files),
        "power_session_count": len(power_sessions),
        "power_packet_count_best_mirror_per_session": sum(item["power_packet_count"] for item in power_sessions),
        "errors": errors,
        "sessions": sessions,
        "files": files,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="capture directory to scan recursively")
    parser.add_argument("--json-out", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    index = build_index(args.root)
    args.json_out.parent.mkdir(parents=True, exist_ok=True)
    args.json_out.write_text(json.dumps(index, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(
        f"files={index['file_count']} sessions={index['session_count']} "
        f"valid_files={index['files_with_valid_frames']} power_sessions={index['power_session_count']} "
        f"errors={len(index['errors'])}"
    )


if __name__ == "__main__":
    main()
