#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import csv
import gzip
import hashlib
import io
import json
import re
import zipfile
from contextlib import contextmanager
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple

LINE_RE = re.compile(r"^(\S+) \| (\d+) bytes? \| (.*)$")
RAW_RE = re.compile(r"(?:^|\]\s+)RAW\s+t=(\d+)\s+len=(\d+)\s+hex=([0-9A-Fa-f ]+)$")
FRAME_RE = re.compile(
    r"(?:^|\]\s+)FRAME\s+t=(\d+)\s+dir=\S+\s+"
    r"addr_raw=([0-9A-Fa-f]+)\s+gateway=([0-9A-Fa-f]+)\s+"
    r"type=([0-9A-Fa-f]+)\s+crc=(ok|bad)\s+payload_len=(\d+)\s+payload=(.*)$"
)
ESCAPE_MAP = {
    0x00: 0x7E,
    0x01: 0x24,
    0x02: 0x23,
    0x03: 0x25,
    0x04: 0xA4,
    0x05: 0xA3,
    0x06: 0xA5,
}


def build_crc_table() -> List[int]:
    table: List[int] = []
    for value in range(256):
        crc = value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0x8408) if (crc & 1) else (crc >> 1)
        table.append(crc & 0xFFFF)
    return table


CRC_TABLE = build_crc_table()


def crc16_tigo(data: bytes, init: int = 0x8408) -> int:
    crc = init
    for byte in data:
        crc = ((crc >> 8) ^ CRC_TABLE[(crc & 0xFF) ^ byte]) & 0xFFFF
    return crc


@dataclass
class GatewayFrame:
    timestamp: str
    from_gateway: bool
    gateway_id: int
    type_code: int
    payload: bytes
    crc_ok: bool
    device_ms: Optional[int] = None


@dataclass
class PowerReport:
    timestamp: str
    node_id: int
    short_addr: int
    gateway_slot_counter: Optional[int]
    slot_counter_report: int
    vin_v: float
    vout_v: float
    iin_a: float
    temp_c: float
    power_in_w: float
    power_out_est_w: float
    rssi: int
    duty_pct: float
    unknown_hex: str
    long_addr: Optional[str] = None
    panel_label: Optional[str] = None


@dataclass
class ActiveCommand:
    timestamp: str
    direction: str
    subcmd: int
    dsn: int
    raw_body_hex: str
    decoded: Dict[str, Any]


@dataclass
class ParseDiagnostics:
    malformed_json_lines: int = 0
    malformed_frame_lines: int = 0
    malformed_raw_lines: int = 0
    invalid_hex_lines: int = 0
    declared_length_mismatches: int = 0

    @property
    def warning_count(self) -> int:
        return sum(asdict(self).values())


@contextmanager
def open_capture_text(path: Path):
    if path.suffix.lower() == ".gz":
        with gzip.open(path, "rt", encoding="utf-8", errors="ignore") as handle:
            yield handle
        return
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            members = [entry for entry in archive.infolist() if not entry.is_dir()]
            if len(members) != 1:
                raise ValueError(f"expected one capture in {path}, found {len(members)}")
            with archive.open(members[0]) as raw_handle:
                with io.TextIOWrapper(raw_handle, encoding="utf-8", errors="ignore") as handle:
                    yield handle
        return
    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        yield handle


def parse_hex_bytes(text: str) -> Optional[bytes]:
    try:
        values = [int(token, 16) for token in text.split()]
    except ValueError:
        return None
    if any(value < 0 or value > 0xFF for value in values):
        return None
    return bytes(values)



def read_log_entries(
    path: Path, diagnostics: Optional[ParseDiagnostics] = None
) -> List[Tuple[str, bytes]]:
    entries: List[Tuple[str, bytes]] = []
    with open_capture_text(path) as handle:
        for line in handle:
            stripped = line.strip()
            match = LINE_RE.match(stripped)
            if match:
                data = parse_hex_bytes(match.group(3))
                if data is None:
                    if diagnostics is not None:
                        diagnostics.invalid_hex_lines += 1
                elif len(data) != int(match.group(2)):
                    if diagnostics is not None:
                        diagnostics.declared_length_mismatches += 1
                else:
                    entries.append((match.group(1), data))
                continue

            match = RAW_RE.search(stripped)
            if match:
                data = parse_hex_bytes(match.group(3))
                if data is None:
                    if diagnostics is not None:
                        diagnostics.invalid_hex_lines += 1
                elif len(data) != int(match.group(2)):
                    if diagnostics is not None:
                        diagnostics.declared_length_mismatches += 1
                else:
                    entries.append((f"t={match.group(1)}ms", data))
            elif "RAW t=" in stripped and diagnostics is not None:
                diagnostics.malformed_raw_lines += 1
    return entries


def read_predecoded_frames(
    path: Path, diagnostics: Optional[ParseDiagnostics] = None
) -> List[GatewayFrame]:
    frames: List[GatewayFrame] = []
    with open_capture_text(path) as handle:
        for line in handle:
            stripped = line.strip()
            if stripped.startswith("{"):
                try:
                    record = json.loads(stripped)
                    source = record.get("frame")
                    if not isinstance(source, dict) and record.get("kind") in ("frame", "interesting_frame"):
                        source = record.get("data")
                    if not isinstance(source, dict) and record.get("kind") == "mqtt":
                        mqtt_data = record.get("data")
                        if (
                            isinstance(mqtt_data, dict)
                            and str(mqtt_data.get("topic", "")).endswith("/raw/frame")
                            and isinstance(mqtt_data.get("value"), dict)
                        ):
                            source = mqtt_data["value"]
                    if isinstance(source, dict):
                        if source.get("valid") is False or source.get("payload_truncated") is True:
                            continue
                        addr_raw = int(source["addr_raw_hex"], 16)
                        payload_hex = source.get("payload_hex", source.get("payload_preview_hex", ""))
                        payload = bytes.fromhex(payload_hex)
                        declared_payload_len = source.get("payload_len")
                        if declared_payload_len is not None and int(declared_payload_len) != len(payload):
                            if diagnostics is not None:
                                diagnostics.declared_length_mismatches += 1
                            continue
                        frames.append(
                            GatewayFrame(
                                timestamp=str(record.get("ts", "")),
                                from_gateway=bool(addr_raw & 0x8000),
                                gateway_id=int(source["gateway_id_hex"], 16),
                                type_code=int(source.get("type_code_hex", source.get("type_hex")), 16),
                                payload=payload,
                                crc_ok=bool(source.get("crc_ok", False)),
                                device_ms=(
                                    int(source["device_ms"])
                                    if source.get("device_ms") is not None
                                    else None
                                ),
                            )
                        )
                        continue
                except json.JSONDecodeError:
                    if diagnostics is not None:
                        diagnostics.malformed_json_lines += 1
                except (KeyError, TypeError, ValueError):
                    if diagnostics is not None:
                        diagnostics.malformed_frame_lines += 1

            match = FRAME_RE.search(stripped)
            if not match:
                if "FRAME t=" in stripped and diagnostics is not None:
                    diagnostics.malformed_frame_lines += 1
                continue
            addr_raw = int(match.group(2), 16)
            payload_text = match.group(7).strip()
            payload = parse_hex_bytes(payload_text) if payload_text else b""
            if payload is None:
                if diagnostics is not None:
                    diagnostics.invalid_hex_lines += 1
                continue
            if len(payload) != int(match.group(6)):
                if diagnostics is not None:
                    diagnostics.declared_length_mismatches += 1
                continue
            frames.append(
                GatewayFrame(
                    timestamp=f"t={match.group(1)}ms",
                    from_gateway=bool(addr_raw & 0x8000),
                    gateway_id=int(match.group(3), 16),
                    type_code=int(match.group(4), 16),
                    payload=payload,
                    crc_ok=(match.group(5) == "ok"),
                )
            )
    return frames


def relative_timestamp_ms(timestamp: str) -> Optional[int]:
    match = re.fullmatch(r"t=(\d+)ms", timestamp)
    return int(match.group(1)) if match else None


def counter16_is_newer_or_equal(value: int, previous: int) -> bool:
    delta = (value - previous) & 0xFFFF
    return delta == 0 or delta < 0x8000


def frame_signature(frame: GatewayFrame) -> Tuple[bool, int, int, bytes, bool]:
    return (
        frame.from_gateway,
        frame.gateway_id,
        frame.type_code,
        frame.payload,
        frame.crc_ok,
    )


def merge_raw_and_predecoded_frames(
    raw_frames: List[GatewayFrame], parsed_frames: List[GatewayFrame]
) -> List[GatewayFrame]:
    parsed_buckets: Dict[Tuple[bool, int, int, bytes, bool], List[Tuple[int, int]]] = {}
    for index, frame in enumerate(parsed_frames):
        frame_ms = relative_timestamp_ms(frame.timestamp)
        if frame_ms is None:
            continue
        parsed_buckets.setdefault(frame_signature(frame), []).append((frame_ms, index))

    matched_parsed = set()
    for raw_frame in raw_frames:
        raw_ms = relative_timestamp_ms(raw_frame.timestamp)
        if raw_ms is None:
            continue
        candidates = parsed_buckets.get(frame_signature(raw_frame))
        if not candidates:
            continue
        position = bisect.bisect_left(candidates, (raw_ms, -1))
        nearest = []
        right = position
        while right < len(candidates) and candidates[right][0] - raw_ms <= 250:
            if candidates[right][1] not in matched_parsed:
                nearest.append(candidates[right])
            right += 1
        left = position - 1
        while left >= 0 and raw_ms - candidates[left][0] <= 250:
            if candidates[left][1] not in matched_parsed:
                nearest.append(candidates[left])
            left -= 1
        if not nearest:
            continue
        candidate_ms, candidate_index = min(nearest, key=lambda item: abs(item[0] - raw_ms))
        matched_parsed.add(candidate_index)

    combined = list(raw_frames)
    combined.extend(
        frame for index, frame in enumerate(parsed_frames) if index not in matched_parsed
    )
    if all(relative_timestamp_ms(frame.timestamp) is not None for frame in combined):
        combined.sort(key=lambda frame: relative_timestamp_ms(frame.timestamp))
    return combined


def read_gateway_frames(
    path: Path, diagnostics: Optional[ParseDiagnostics] = None
) -> List[GatewayFrame]:
    """Read all historical logger formats without double-counting parsed mirrors."""
    if path.suffix.lower() in (".jsonl", ".zip"):
        return read_predecoded_frames(path, diagnostics)
    entries = read_log_entries(path, diagnostics)
    raw_frames = list(iter_gateway_frames(entries))
    parsed_frames = read_predecoded_frames(path, diagnostics)
    if not raw_frames:
        return parsed_frames
    if not parsed_frames:
        return raw_frames

    # Listener logs can switch RAW logging on/off while continuing to emit FRAME
    # lines. Merge both streams and remove only timestamp-near exact duplicates.
    return merge_raw_and_predecoded_frames(raw_frames, parsed_frames)


def build_stream(entries: Iterable[Tuple[str, bytes]]) -> Tuple[bytes, List[str]]:
    stream = bytearray()
    timestamps: List[str] = []
    for ts, data in entries:
        stream.extend(data)
        timestamps.extend([ts] * len(data))
    return bytes(stream), timestamps



def unescape_gateway_payload(raw: bytes) -> Optional[bytes]:
    out = bytearray()
    idx = 0
    while idx < len(raw):
        byte = raw[idx]
        if byte == 0x7E:
            if idx + 1 >= len(raw):
                return None
            esc = raw[idx + 1]
            if esc not in ESCAPE_MAP:
                return None
            out.append(ESCAPE_MAP[esc])
            idx += 2
        else:
            out.append(byte)
            idx += 1
    return bytes(out)


def iter_gateway_frames(entries: List[Tuple[str, bytes]]) -> Iterator[GatewayFrame]:
    stream, timestamps = build_stream(entries)
    start = 0
    while True:
        frame_start = stream.find(b"\x7E\x07", start)
        if frame_start < 0:
            return
        frame_end = stream.find(b"\x7E\x08", frame_start + 2)
        if frame_end < 0:
            return
        timestamp = timestamps[frame_start]
        raw_body = stream[frame_start + 2 : frame_end]
        body = unescape_gateway_payload(raw_body)
        start = frame_end + 2

        if body is None or len(body) < 6:
            continue

        data = body[:-2]
        checksum = int.from_bytes(body[-2:], "little")
        addr_raw = int.from_bytes(data[0:2], "big")
        type_code = int.from_bytes(data[2:4], "big")
        payload = data[4:]

        yield GatewayFrame(
            timestamp=timestamp,
            from_gateway=bool(addr_raw & 0x8000),
            gateway_id=addr_raw & 0x7FFF,
            type_code=type_code,
            payload=payload,
            crc_ok=(checksum == crc16_tigo(data)),
        )


def parse_rx_status_and_blob(payload: bytes) -> Optional[Tuple[Dict[str, Any], bytes]]:
    if len(payload) < 5:
        return None

    status_flags = int.from_bytes(payload[0:2], "big")
    idx = 2
    result: Dict[str, Any] = {"status_type": status_flags}

    def take(count: int) -> Optional[bytes]:
        nonlocal idx
        if idx + count > len(payload):
            return None
        value = payload[idx : idx + count]
        idx += count
        return value

    if (status_flags & 0x0001) == 0:
        value = take(1)
        if value is None:
            return None
        result["rx_buffers_used"] = value[0]

    if (status_flags & 0x0002) == 0:
        value = take(1)
        if value is None:
            return None
        result["tx_buffers_free"] = value[0]

    if (status_flags & 0x0004) == 0:
        value = take(2)
        if value is None:
            return None
        result["unknown_a"] = value.hex()

    if (status_flags & 0x0008) == 0:
        value = take(2)
        if value is None:
            return None
        result["unknown_b"] = value.hex()

    if (status_flags & 0x0010) == 0:
        value = take(1)
        if value is None:
            return None
        result["packet_counter_high"] = value[0]

    value = take(1)
    if value is None:
        return None
    result["packet_counter_low"] = value[0]

    value = take(2)
    if value is None:
        return None
    result["slot_counter_gateway"] = int.from_bytes(value, "big")

    return result, payload[idx:]


def split_pv_packets(blob: bytes) -> List[Dict[str, Any]]:
    packets: List[Dict[str, Any]] = []
    idx = 0
    while idx + 7 <= len(blob):
        ptype = blob[idx]
        node_id = int.from_bytes(blob[idx + 1 : idx + 3], "big")
        short_addr = int.from_bytes(blob[idx + 3 : idx + 5], "big")
        dsn = blob[idx + 5]
        data_len = blob[idx + 6]
        end = idx + 7 + data_len
        if end > len(blob):
            break
        data = blob[idx + 7 : end]
        packets.append(
            {
                "ptype": ptype,
                "node_id": node_id,
                "short_addr": short_addr,
                "dsn": dsn,
                "data_len": data_len,
                "data": data,
            }
        )
        idx = end
    return packets


def decode_power_report(data: bytes, *, iin_scale: float) -> Optional[Dict[str, Any]]:
    if len(data) != 13:
        return None
    vin_raw = ((data[0] << 4) | (data[1] >> 4)) & 0x0FFF
    vout_raw = (((data[1] & 0x0F) << 8) | data[2]) & 0x0FFF
    duty_raw = data[3]
    iin_raw = ((data[4] << 4) | (data[5] >> 4)) & 0x0FFF
    temp_raw = (((data[5] & 0x0F) << 8) | data[6]) & 0x0FFF
    vin_v = vin_raw * 0.05
    vout_v = vout_raw * 0.10
    iin_a = iin_raw * iin_scale
    temp_c = temp_raw * 0.10
    return {
        "vin_v": round(vin_v, 3),
        "vout_v": round(vout_v, 3),
        "iin_a": round(iin_a, 4),
        "temp_c": round(temp_c, 1),
        "power_in_w": round(vin_v * iin_a, 3),
        "power_out_est_w": round(vout_v * iin_a, 3),
        "duty_pct": round(duty_raw / 255.0 * 100.0, 1),
        "slot_counter_report": int.from_bytes(data[10:12], "big"),
        "rssi": data[12],
        "unknown_hex": data[7:10].hex(),
    }


def decode_topology_report(data: bytes) -> Optional[Dict[str, Any]]:
    if len(data) != 23:
        return None
    return {
        "short_addr_reported": int.from_bytes(data[0:2], "big"),
        "node_id_reported": int.from_bytes(data[2:4], "big"),
        "parent_short_addr": int.from_bytes(data[4:6], "big"),
        "unknown_6_8": data[6:8].hex(),
        "long_addr": data[8:16].hex().upper(),
        "link_or_rssi": data[16],
        "hop_depth": data[17],
        "metric": int.from_bytes(data[18:20], "big"),
        "tail_hex": data[20:].hex().upper(),
    }


def decode_node_diagnostic(data: bytes) -> Optional[Dict[str, Any]]:
    if len(data) != 49:
        return None
    return {
        "opcode": data[0],
        "pan_id_a_hex": data[1:3].hex().upper(),
        "channel_a": data[3],
        "pan_id_b_hex": data[6:8].hex().upper(),
        "channel_b": data[8],
        "reported_short_addr": int.from_bytes(data[41:43], "big"),
        "tail_hex": data[43:].hex().upper(),
        "raw_hex": data.hex().upper(),
    }


def decode_node_table_response(data: bytes) -> Optional[Dict[str, Any]]:
    if len(data) < 4:
        return None
    start_index = int.from_bytes(data[0:2], "big")
    entry_count = int.from_bytes(data[2:4], "big")
    expected_length = 4 + entry_count * 10
    if len(data) != expected_length:
        return None
    pos = 4
    entries: List[Dict[str, Any]] = []
    for _ in range(entry_count):
        if pos + 10 > len(data):
            break
        long_addr = data[pos : pos + 8].hex().upper()
        node_id_raw = int.from_bytes(data[pos + 8 : pos + 10], "big")
        entries.append(
            {
                "long_addr": long_addr,
                "node_id_raw": node_id_raw,
                "node_id": node_id_raw & 0x7FFF,
                "pending": bool(node_id_raw & 0x8000),
            }
        )
        pos += 10
    return {"start_index": start_index, "entry_count": entry_count, "entries": entries}


def decode_node_table_write(data: bytes) -> Optional[Dict[str, Any]]:
    if not data:
        return None
    entry_count = data[0]
    if len(data) != 1 + entry_count * 10:
        return None
    decoded = decode_node_table_response(b"\x00\x00" + entry_count.to_bytes(2, "big") + data[1:])
    if decoded is None:
        return None
    return {"entry_count": entry_count, "entries": decoded["entries"]}


def decode_radio_descriptor(data: bytes) -> Optional[Dict[str, Any]]:
    if len(data) not in (36, 37):
        return None
    descriptor = data[:36]
    result = {
        "channel": int.from_bytes(descriptor[0:2], "big"),
        "pan_id_hex": descriptor[2:4].hex().upper(),
        "protocol_hex": descriptor[4:8].hex().upper(),
        "epoch_or_network_id_hex": descriptor[8:12].hex().upper(),
        "marker_hex": descriptor[12:15].hex().upper(),
        "probable_network_key_hex": descriptor[15:31].hex().upper(),
        "tail_hex": descriptor[31:36].hex().upper(),
        "fingerprint_sha256_12": hashlib.sha256(descriptor).hexdigest()[:12].upper(),
        "raw_descriptor_hex": descriptor.hex().upper(),
    }
    if len(data) == 37:
        result["result"] = data[36]
        result["result_state"] = {
            0x00: "stable",
            0x01: "learn_busy",
        }.get(data[36], "unknown")
    return result


def decode_active_command(frame: GatewayFrame) -> Optional[ActiveCommand]:
    if frame.type_code not in (0x0B0F, 0x0B10) or len(frame.payload) < 5:
        return None
    direction = "request" if frame.type_code == 0x0B0F else "response"
    status_or_flags = int.from_bytes(frame.payload[0:2], "big")
    subcmd = int.from_bytes(frame.payload[2:4], "big")
    dsn = frame.payload[4]
    body = frame.payload[5:]
    decoded: Dict[str, Any] = {"status_or_flags": status_or_flags, "body_hex": body.hex().upper()}
    if direction == "response":
        decoded["tx_buffers_free"] = frame.payload[1]

    if (direction == "request" and subcmd == 0x000D) or (direction == "response" and subcmd == 0x000E):
        if direction == "response":
            descriptor = decode_radio_descriptor(body)
            if descriptor:
                decoded["radio_descriptor"] = descriptor
        elif len(body) >= 2 and body[0:2] == b"\x01\x00":
            descriptor = decode_radio_descriptor(body[2:])
            if descriptor:
                decoded["radio_descriptor_write"] = descriptor
        elif len(body) == 2:
            decoded["radio_descriptor_selector"] = int.from_bytes(body, "big")
    elif direction == "response" and subcmd == 0x0027:
        node_table = decode_node_table_response(body)
        if node_table:
            decoded["node_table"] = node_table
    elif direction == "response" and subcmd == 0x002F and len(body) >= 9:
        confirmed_nodes = int.from_bytes(body[5:7], "big")
        expected_nodes = int.from_bytes(body[7:9], "big")
        decoded["network_status"] = {
            "mode_or_state": body[0],
            "countdown_or_timer": int.from_bytes(body[1:3], "big"),
            "unknown_u16": int.from_bytes(body[3:5], "big"),
            "confirmed_nodes": confirmed_nodes,
            "expected_nodes": expected_nodes,
            # Compatibility aliases used by older reports and firmware.
            "active_nodes": confirmed_nodes,
            "configured_nodes": expected_nodes,
        }
    elif subcmd == 0x0041:
        command_41: Dict[str, Any] = {"payload_len": len(body), "body_hex": body.hex().upper()}
        if len(body) == 24:
            command_41.update({
                "target_u16": int.from_bytes(body[0:2], "big"),
                "target_u32": int.from_bytes(body[2:6], "big"),
                "marker_hex": body[6:8].hex().upper(),
                "network_block_hex": body[8:24].hex().upper(),
            })
        decoded["routed_network_control"] = command_41
        decoded["unknown_41"] = command_41
    elif subcmd == 0x0026 and direction == "request":
        if len(body) in (2, 4):
            request = {"start_index": int.from_bytes(body[0:2], "big")}
            if len(body) == 4:
                request["max_entries"] = int.from_bytes(body[2:4], "big")
            decoded["node_table_request"] = request
    elif subcmd == 0x002E:
        decoded["network_status_request"] = {"body_hex": body.hex().upper()}
    elif subcmd == 0x002D:
        learn_control: Dict[str, Any] = {"body_hex": body.hex().upper()}
        if len(body) >= 8 and body[0:2] == b"\xBA\xBE":
            learn_control.update({
                "guard_hex": "BABE",
                "action": body[2],
                "countdown_seconds": int.from_bytes(body[3:5], "big"),
                "expected_nodes": int.from_bytes(body[5:7], "big"),
                "option": body[7],
                "trailer_hex": body[8:].hex().upper(),
            })
        decoded["learn_control"] = learn_control
        decoded["unknown_2d"] = learn_control
    elif subcmd == 0x0029 and direction == "request":
        table_write = decode_node_table_write(body)
        if table_write:
            decoded["node_table_write"] = table_write
    elif subcmd == 0x002B and direction == "request":
        decoded["node_table_reset"] = {"guard_hex": body.hex().upper()}
    elif subcmd in (0x0022, 0x0023):
        selector: Dict[str, Any] = {"body_hex": body.hex().upper()}
        if direction == "request" and len(body) == 2:
            selector["read_selector"] = int.from_bytes(body, "big")
        elif direction == "request" and len(body) == 3:
            selector["operation"] = body[0]
            selector["argument"] = int.from_bytes(body[1:3], "big")
        elif direction == "response" and len(body) == 1:
            selector["value"] = body[0]
        decoded["selector"] = selector

    return ActiveCommand(
        timestamp=frame.timestamp,
        direction=direction,
        subcmd=subcmd,
        dsn=dsn,
        raw_body_hex=body.hex().upper(),
        decoded=decoded,
    )


def load_panel_map(path: Optional[Path]) -> Dict[str, str]:
    if path is None:
        return {}
    obj = json.loads(path.read_text(encoding="utf-8"))
    return {str(k).upper(): str(v) for k, v in obj.items()}


def analyze_log(logfile: Path, *, iin_scale: float, panel_map: Dict[str, str]) -> Dict[str, Any]:
    frames = [f for f in read_gateway_frames(logfile) if f.crc_ok]

    frame_type_counts: Dict[str, int] = {}
    rx_status_counts: Dict[str, int] = {}
    pv_packet_counts: Dict[str, int] = {}
    active_subcmd_counts: Dict[str, int] = {}
    long_addr_by_node: Dict[int, str] = {}
    pending_by_node: Dict[int, bool] = {}
    topology_reports: List[Dict[str, Any]] = []
    node_diagnostics: List[Dict[str, Any]] = []
    active_timeline: List[Dict[str, Any]] = []
    all_power_rows: List[PowerReport] = []
    latest_power_by_node: Dict[Tuple[int, int], PowerReport] = {}
    last_network_status: Optional[Dict[str, Any]] = None

    for frame in frames:
        key = f"0x{frame.type_code:04X}"
        frame_type_counts[key] = frame_type_counts.get(key, 0) + 1

        if frame.type_code == 0x0149:
            parsed = parse_rx_status_and_blob(frame.payload)
            if parsed is None:
                continue
            status, pv_blob = parsed
            status_key = f"0x{status['status_type']:04X}"
            rx_status_counts[status_key] = rx_status_counts.get(status_key, 0) + 1
            for packet in split_pv_packets(pv_blob):
                ptype = packet["ptype"]
                ptype_key = f"0x{ptype:02X}"
                pv_packet_counts[ptype_key] = pv_packet_counts.get(ptype_key, 0) + 1
                data = packet["data"]
                if ptype == 0x27:
                    decoded = decode_node_table_response(data)
                    if decoded:
                        for entry in decoded["entries"]:
                            long_addr_by_node[entry["node_id"]] = entry["long_addr"]
                            pending_by_node[entry["node_id"]] = entry["pending"]
                elif ptype == 0x09:
                    decoded = decode_topology_report(data)
                    if decoded:
                        decoded["timestamp"] = frame.timestamp
                        topology_reports.append(decoded)
                        long_addr_by_node[decoded["node_id_reported"]] = decoded["long_addr"]
                elif ptype == 0x18:
                    decoded = decode_node_diagnostic(data)
                    if decoded:
                        decoded["timestamp"] = frame.timestamp
                        decoded["node_id"] = packet["node_id"]
                        decoded["short_addr"] = packet["short_addr"]
                        node_diagnostics.append(decoded)
                elif ptype == 0x31:
                    decoded = decode_power_report(data, iin_scale=iin_scale)
                    if not decoded:
                        continue
                    long_addr = long_addr_by_node.get(packet["node_id"])
                    panel_label = panel_map.get(long_addr) if long_addr else None
                    row = PowerReport(
                        timestamp=frame.timestamp,
                        node_id=packet["node_id"],
                        short_addr=packet["short_addr"],
                        gateway_slot_counter=status.get("slot_counter_gateway"),
                        slot_counter_report=decoded["slot_counter_report"],
                        vin_v=decoded["vin_v"],
                        vout_v=decoded["vout_v"],
                        iin_a=decoded["iin_a"],
                        temp_c=decoded["temp_c"],
                        power_in_w=decoded["power_in_w"],
                        power_out_est_w=decoded["power_out_est_w"],
                        rssi=decoded["rssi"],
                        duty_pct=decoded["duty_pct"],
                        unknown_hex=decoded["unknown_hex"],
                        long_addr=long_addr,
                        panel_label=panel_label,
                    )
                    all_power_rows.append(row)
                    key = (row.node_id, row.short_addr)
                    prev = latest_power_by_node.get(key)
                    if prev is None or counter16_is_newer_or_equal(
                        row.slot_counter_report, prev.slot_counter_report
                    ):
                        latest_power_by_node[key] = row

        active = decode_active_command(frame)
        if active is not None:
            sub_key = f"0x{active.subcmd:02X}:{active.direction}"
            active_subcmd_counts[sub_key] = active_subcmd_counts.get(sub_key, 0) + 1
            record = asdict(active)
            if active.subcmd == 0x0027 and "node_table" in active.decoded:
                for entry in active.decoded["node_table"]["entries"]:
                    long_addr_by_node[entry["node_id"]] = entry["long_addr"]
                    pending_by_node[entry["node_id"]] = entry["pending"]
            if active.subcmd == 0x002F and "network_status" in active.decoded:
                last_network_status = active.decoded["network_status"]
            active_timeline.append(record)

    labeled_node_table = []
    for node_id, long_addr in sorted(long_addr_by_node.items()):
        labeled_node_table.append(
            {
                "node_id": node_id,
                "long_addr": long_addr,
                "pending": pending_by_node.get(node_id),
                "panel_label": panel_map.get(long_addr),
            }
        )
        # backfill labels into latest power rows if we learned addresses later
        for row in latest_power_by_node.values():
            if row.node_id == node_id and row.long_addr is None:
                row.long_addr = long_addr
                row.panel_label = panel_map.get(long_addr)
        for row in all_power_rows:
            if row.node_id == node_id and row.long_addr is None:
                row.long_addr = long_addr
                row.panel_label = panel_map.get(long_addr)

    phase = "telemetry"
    if not all_power_rows and active_timeline:
        phase = "wakeup_or_discovery"
    elif active_timeline and all_power_rows:
        phase = "mixed"

    return {
        "file": str(logfile),
        "gateway_id_hex": f"0x{frames[0].gateway_id:04X}" if frames else None,
        "frame_count_crc_ok": len(frames),
        "frame_type_counts": frame_type_counts,
        "rx_status_counts": rx_status_counts,
        "pv_packet_counts": pv_packet_counts,
        "active_subcmd_counts": active_subcmd_counts,
        "phase": phase,
        "node_table": labeled_node_table,
        "topology_reports": topology_reports,
        "node_diagnostics": node_diagnostics,
        "latest_power": [asdict(row) for _, row in sorted(latest_power_by_node.items())],
        "all_power_reports": [asdict(row) for row in all_power_rows],
        "active_timeline": active_timeline,
        "last_network_status": last_network_status,
    }


def write_csv(path: Path, rows: List[Dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode Tigo TAP/CCA capture logs (telemetry + wake-up active commands).")
    parser.add_argument("logfile", type=Path)
    parser.add_argument("--iin-scale", type=float, default=0.0056)
    parser.add_argument("--panel-map-json", type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--latest-csv", type=Path)
    parser.add_argument("--timeline-csv", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    panel_map = load_panel_map(args.panel_map_json)
    result = analyze_log(args.logfile, iin_scale=args.iin_scale, panel_map=panel_map)
    if args.json_out:
        args.json_out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    if args.latest_csv:
        write_csv(args.latest_csv, result["latest_power"])
    if args.timeline_csv:
        # Flatten active timeline for CSV
        rows = []
        for item in result["active_timeline"]:
            rows.append(
                {
                    "timestamp": item["timestamp"],
                    "direction": item["direction"],
                    "subcmd_hex": f"0x{item['subcmd']:02X}",
                    "dsn": item["dsn"],
                    "raw_body_hex": item["raw_body_hex"],
                    "decoded_json": json.dumps(item["decoded"], separators=(",", ":")),
                }
            )
        write_csv(args.timeline_csv, rows)
    print(json.dumps(
        {
            "file": result["file"],
            "phase": result["phase"],
            "gateway_id_hex": result["gateway_id_hex"],
            "frame_type_counts": result["frame_type_counts"],
            "active_subcmd_counts": result["active_subcmd_counts"],
            "node_count": len(result["node_table"]),
            "power_reports": len(result["all_power_reports"]),
        },
        indent=2,
    ))


if __name__ == "__main__":
    main()
