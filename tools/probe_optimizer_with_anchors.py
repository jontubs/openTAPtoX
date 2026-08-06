#!/usr/bin/env python3
"""Probe pending optimizers while preserving RF-proven anchors."""

from __future__ import annotations

import argparse
import json
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


def timestamp() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://opentaptox-esp32c6.local")
    parser.add_argument("--seconds-per-optimizer", type=float, default=300.0)
    parser.add_argument("--pv-off-seconds", type=float, default=0.0)
    parser.add_argument(
        "--pv-off-after-learn-seconds",
        type=float,
        default=0.0,
        help="Hold the CCA-style PV-off state after table/learn setup.",
    )
    parser.add_argument(
        "--pv-run-body",
        choices=("0000", "empty"),
        default="0000",
        help="Packet 0x22 payload used to de-assert PV-off.",
    )
    parser.add_argument(
        "--target-entry",
        choices=("pending", "low"),
        default="pending",
        help="Write the target as a pending ID or as its legacy low ID.",
    )
    parser.add_argument("--target-label", action="append", default=[])
    parser.add_argument(
        "--join-seed",
        required=True,
        help="profile-specific 24-byte hex value; keep it local and never commit it",
    )
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    join_seed = args.join_seed.strip().upper()
    if len(join_seed) != 48 or any(char not in "0123456789ABCDEF" for char in join_seed):
        parser.error("--join-seed must contain exactly 48 hexadecimal characters")

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    base_url = args.base_url.rstrip("/")

    def write(kind: str, data: object) -> None:
        row = {
            "ts": timestamp(),
            "elapsed_s": round(time.monotonic() - started, 3),
            "kind": kind,
            "data": data,
        }
        with output.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(row, separators=(",", ":"), sort_keys=True) + "\n")

    def request(path: str, method: str = "GET") -> dict:
        req = urllib.request.Request(
            base_url + path,
            data=b"" if method == "POST" else None,
            method=method,
        )
        with urllib.request.urlopen(req, timeout=6) as response:
            return json.load(response)

    def status() -> dict:
        return request("/api/status")

    def wait_idle(timeout: float = 15.0) -> dict:
        deadline = time.monotonic() + timeout
        last: dict = {}
        while time.monotonic() < deadline:
            try:
                last = status()
                if not last.get("command_busy"):
                    return last
            except Exception:
                pass
            time.sleep(0.2)
        raise TimeoutError(f"TAP command remained busy: {last.get('command_state')}")

    def command(path: str, method: str = "GET") -> dict:
        deadline = time.monotonic() + 20.0
        last_error = ""
        while time.monotonic() < deadline:
            wait_idle()
            try:
                result = request(path, method)
                if result.get("ok"):
                    completed = wait_idle()
                    write(
                        "command",
                        {
                            "path": path,
                            "response": result,
                            "command_state": completed.get("command_state"),
                            "ack_status": completed.get("last_pv_ack_status_hex"),
                        },
                    )
                    return completed
                last_error = str(result)
            except Exception as exc:
                last_error = str(exc)
            time.sleep(0.3)
        raise RuntimeError(f"command failed: {path}: {last_error}")

    panel_rows = request("/api/panel-map").get("panel_map", [])
    panels = []
    for index, item in enumerate(panel_rows):
        long_addr = str(item.get("long_addr", "")).upper()
        if len(long_addr) != 16:
            continue
        panels.append(
            {
                "node_id": index + 2,
                "label": str(item.get("label", "")) or f"A{index + 1}",
                "long_addr": long_addr,
            }
        )
    panel_by_id = {panel["node_id"]: panel for panel in panels}

    initial_nodes = request("/api/node-map").get("nodes", [])
    confirmed = {
        int(node["node_id"])
        for node in initial_nodes
        if not node.get("pending", True)
        and (node.get("rf_confirmed") or node.get("has_power"))
        and int(node.get("node_id", 0)) in panel_by_id
    }
    if not confirmed:
        raise RuntimeError("at least one RF-proven anchor is required")

    requested_labels = {label.upper() for label in args.target_label}
    targets = [
        panel
        for panel in panels
        if panel["node_id"] not in confirmed
        and (not requested_labels or panel["label"].upper() in requested_labels)
    ]
    if requested_labels - {panel["label"].upper() for panel in targets}:
        raise RuntimeError("one or more requested target labels are unavailable or already confirmed")

    def table_body(included: list[int], confirmed_ids: set[int]) -> str:
        body = f"{len(included):02X}"
        for node_id in included:
            panel = panel_by_id[node_id]
            raw_id = node_id if node_id in confirmed_ids else 0x8000 | node_id
            body += panel["long_addr"] + f"{raw_id:04X}"
        return body

    def pv_path(subcmd: int, body_hex: str) -> str:
        return "/api/command/pv-subcmd?" + urllib.parse.urlencode(
            {"subcmd": f"0x{subcmd:02X}", "body_hex": body_hex}
        )

    def start_reduced_network(target_id: int) -> None:
        included = sorted(confirmed | {target_id})
        command("/api/command/hold-learn?confirm=HOLD_LEARN", "POST")
        command(pv_path(0x2B, "BABE"))
        command(pv_path(0x2D, "BABE02038400000100"))
        command(pv_path(0x22, "0001"))
        if args.pv_off_seconds > 0:
            write("pv_off_hold", {"seconds": args.pv_off_seconds})
            time.sleep(args.pv_off_seconds)
        table_confirmed = set(confirmed)
        if args.target_entry == "low":
            table_confirmed.add(target_id)
        command(pv_path(0x29, table_body(included, table_confirmed)))
        command("/api/command/node-table")
        readback = request("/api/node-map").get("nodes", [])
        write("reduced_table_readback", {"target": target_id, "nodes": readback})
        expected = len(included)
        command(pv_path(0x2D, f"BABE020384{expected:04X}0100"))
        if args.pv_off_after_learn_seconds > 0:
            # A CCA refreshes an asserted PV-off state with a short on/off edge.
            command(pv_path(0x22, "0000"))
            time.sleep(1.0)
            command(pv_path(0x22, "0001"))
            write(
                "pv_off_after_learn_hold",
                {"seconds": args.pv_off_after_learn_seconds},
            )
            time.sleep(args.pv_off_after_learn_seconds)
        run_body = "" if args.pv_run_body == "empty" else args.pv_run_body
        command(pv_path(0x22, run_body))
        command(pv_path(0x41, join_seed))

    def target_has_rf_evidence(target_id: int) -> bool:
        nodes = request("/api/node-map").get("nodes", [])
        return any(
            int(node.get("node_id", 0)) == target_id
            and (node.get("rf_confirmed") or node.get("has_power"))
            for node in nodes
        )

    def restore_full_table() -> None:
        all_ids = sorted(panel_by_id)
        command("/api/command/hold-learn?confirm=HOLD_LEARN", "POST")
        command(pv_path(0x2B, "BABE"))
        command(pv_path(0x2D, "BABE02038400000100"))
        command(pv_path(0x22, "0001"))
        command(pv_path(0x29, table_body(all_ids, confirmed)))
        command("/api/command/node-table")
        command(pv_path(0x2D, f"BABE020384{len(all_ids):04X}0100"))
        command(pv_path(0x22, "0000"))
        command(pv_path(0x41, join_seed))
        command("/api/command/force-learn")

    write(
        "probe_start",
        {
            "seconds_per_optimizer": args.seconds_per_optimizer,
            "pv_off_seconds": args.pv_off_seconds,
            "pv_off_after_learn_seconds": args.pv_off_after_learn_seconds,
            "pv_run_body": args.pv_run_body,
            "target_entry": args.target_entry,
            "anchors": sorted(confirmed),
            "targets": targets,
        },
    )
    try:
        for target in targets:
            target_id = target["node_id"]
            write("target_start", {"target": target, "anchors": sorted(confirmed)})
            start_reduced_network(target_id)
            target_started = time.monotonic()
            next_status = target_started
            next_seed = target_started + 60.0
            info_sent = False
            detected = False
            while time.monotonic() - target_started < args.seconds_per_optimizer:
                now = time.monotonic()
                if now >= next_status:
                    command("/api/command/network-status")
                    current = status()
                    detected = target_has_rf_evidence(target_id)
                    write(
                        "observation",
                        {"target": target, "status": current, "detected": detected},
                    )
                    if detected:
                        confirmed.add(target_id)
                        break
                    next_status = now + 10.0
                if now >= next_seed:
                    command(pv_path(0x41, join_seed))
                    next_seed = now + 60.0
                if not info_sent and now - target_started >= 90.0:
                    command(
                        "/api/command/node-text?"
                        + urllib.parse.urlencode(
                            {"nodeId": str(target_id), "text": "^00Info", "appendCr": "true"}
                        )
                    )
                    info_sent = True
                time.sleep(0.2)
            write(
                "target_result",
                {
                    "target": target,
                    "detected": detected,
                    "confirmed": sorted(confirmed),
                    "status": status(),
                },
            )
    finally:
        try:
            restore_full_table()
            write("restore_full_table", {"ok": True, "confirmed": sorted(confirmed), "status": status()})
        except Exception as exc:
            write("restore_full_table", {"ok": False, "error": str(exc), "confirmed": sorted(confirmed)})
        write("probe_stop", {})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
