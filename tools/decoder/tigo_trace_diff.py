#!/usr/bin/env python3
"""Response-aware differential comparison for Tigo TAP/CCA traces."""

from __future__ import annotations

import argparse
import difflib
import json
import re
from collections import Counter, defaultdict, deque
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Optional

try:
    from .tigo_tap_decoder_v2 import (
        GatewayFrame,
        decode_active_command,
        decode_power_report,
        decode_topology_report,
        parse_rx_status_and_blob,
        read_gateway_frames,
        relative_timestamp_ms,
        split_pv_packets,
    )
except ImportError:
    from tigo_tap_decoder_v2 import (
        GatewayFrame,
        decode_active_command,
        decode_power_report,
        decode_topology_report,
        parse_rx_status_and_blob,
        read_gateway_frames,
        relative_timestamp_ms,
        split_pv_packets,
    )


EXPECTED_TYPES = {
    0x0014: 0x0015,
    0x0038: 0x0039,
    0x003A: 0x003B,
    0x003C: 0x003D,
    0x0010: 0x0011,
    0x0012: 0x0013,
    0x000A: 0x000B,
    0x000E: 0x000F,
    0x0E02: 0x0006,
    0x0B00: 0x0B01,
    0x0148: 0x0149,
    0x0B0F: 0x0B10,
}

NODE_SUBCOMMANDS = {0x06, 0x13, 0x17}
READ_ONLY_TYPES = {0x0038, 0x003A, 0x000A, 0x000E, 0x0E02, 0x0148}
STATE_CHANGING_TYPES = {0x0010, 0x0012, 0x0014, 0x003C}
# 0x0D is a descriptor read. Node text/opcode probes (0x06/0x17) are active
# RF operations, but the observed Version/Info/0x0F forms are not themselves
# configuration mutations. Keep those separate from actual state changes.
STATE_CHANGING_SUBCOMMANDS = {0x13, 0x22, 0x29, 0x2B, 0x2D, 0x41}


def request_is_state_changing(
    type_code: int, payload: bytes, subcmd: Optional[int] = None
) -> bool:
    if type_code == 0x0B00 and payload in (b"\x00", b"\x01"):
        return True
    return type_code in STATE_CHANGING_TYPES or (
        subcmd in STATE_CHANGING_SUBCOMMANDS if subcmd is not None else False
    )
KNOWN_RF_PACKET_TYPES = {0x09, 0x0E, 0x18, 0x27, 0x31}
ALIGNMENT_COVERAGE_WARNING_THRESHOLD = 0.5
CAPTURE_ASYMMETRY_WARNING_THRESHOLD = 4.0
CAPTURE_EDGE_PENDING_MS = 5000.0
SERIAL_TX_RE = re.compile(r"OTX event t=(\d+) pv-subcmd 0x([0-9A-Fa-f]{2}) tx=([0-9A-Fa-f]+)")
SERIAL_SHORT_ACK_RE = re.compile(r"OTX event t=(\d+) pv-subcmd 0x([0-9A-Fa-f]{2}) short ack raw=([0-9A-Fa-f]+)")
SERIAL_ACTIVE_ACK_RE = re.compile(
    r"OTX event t=(\d+) pv-subcmd 0x([0-9A-Fa-f]{2}) ack status=0x([0-9A-Fa-f]{2}) "
    r"rsp=0x([0-9A-Fa-f]{2}) body=([0-9A-Fa-f]*)"
)


@dataclass
class TraceEvent:
    index: int
    time_ms: float
    direction: str
    category: str
    signature: str
    gateway: str
    frame_type: int
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class Transaction:
    request_index: int
    request_time_ms: float
    request_signature: str
    request_type: int
    gateway: str
    expected_type: int
    dsn: Optional[int]
    subcmd: Optional[int]
    responses: list[str] = field(default_factory=list)
    response_time_ms: Optional[float] = None
    latency_ms: Optional[float] = None
    outcome: str = "pending"
    state_changing: bool = False

    @property
    def alignment_token(self) -> str:
        return f"{self.request_signature} -> {self.outcome}"


@dataclass
class TraceModel:
    path: str
    capture_mode: str
    frame_count: int
    discarded_crc_frame_count: int
    events: list[TraceEvent]
    transactions: list[Transaction]
    unknown_rf_packets: list[dict[str, Any]]
    first_release_ms: Optional[float]
    first_power_ms: Optional[float]
    first_rf_response_ms: Optional[float]
    gateway_aliases: dict[int, str]
    node_aliases: dict[int, str]


@dataclass
class FrameClock:
    first_iso: Optional[datetime] = None
    device_offset_ms: Optional[float] = None
    last_device_ms: Optional[int] = None


def wall_frame_time_ms(frame: GatewayFrame, clock: FrameClock) -> float:
    relative = relative_timestamp_ms(frame.timestamp)
    if relative is not None:
        return float(relative)
    try:
        current = datetime.fromisoformat(frame.timestamp.replace("Z", "+00:00"))
    except (TypeError, ValueError):
        return 0.0
    if clock.first_iso is None:
        clock.first_iso = current
    return (current - clock.first_iso).total_seconds() * 1000.0


def frame_time_ms(frame: GatewayFrame, clock: FrameClock) -> float:
    wall_ms = wall_frame_time_ms(frame, clock)
    if frame.device_ms is None:
        return wall_ms
    if (
        clock.device_offset_ms is None or
        (clock.last_device_ms is not None and frame.device_ms < clock.last_device_ms)
    ):
        # Anchor every boot-local millis epoch to the capture wall clock. This
        # preserves chronology across ESP reboots while retaining local latency.
        clock.device_offset_ms = wall_ms - float(frame.device_ms)
    clock.last_device_ms = frame.device_ms
    return clock.device_offset_ms + float(frame.device_ms)


def scan_identities(frames: Iterable[GatewayFrame]) -> tuple[dict[int, str], dict[int, str]]:
    gateways: dict[int, str] = {0x0000: "BROADCAST", 0x120A: "TEMP_120A", 0x1235: "ENUM"}
    nodes: dict[int, str] = {}
    for frame in frames:
        if frame.type_code in (0x0039, 0x003B) and len(frame.payload) >= 10:
            eui = frame.payload[:8].hex().upper()
            advertised = int.from_bytes(frame.payload[8:10], "big")
            gateways[frame.gateway_id] = f"TAP[{eui}]"
            gateways[advertised] = f"TAP[{eui}]"
        command = decode_active_command(frame)
        if command and command.direction == "response" and command.subcmd == 0x27:
            table = command.decoded.get("node_table", {})
            for entry in table.get("entries", []):
                nodes[int(entry["node_id"])] = f"NODE[{entry['long_addr']}]"
        if frame.type_code != 0x0149:
            continue
        parsed = parse_rx_status_and_blob(frame.payload)
        if not parsed:
            continue
        _, blob = parsed
        for packet in split_pv_packets(blob):
            if packet["ptype"] == 0x09 and len(packet["data"]) >= 16:
                eui = packet["data"][8:16].hex().upper()
                node_id = int.from_bytes(packet["data"][2:4], "big") & 0x7FFF
                nodes[node_id] = f"NODE[{eui}]"
    return gateways, nodes


def read_serial_diagnostic_frames(path: Path, gateway_id: int = 0x1209) -> list[GatewayFrame]:
    """Recover response-aware PV transactions from historical ESP event logs."""
    frames: list[GatewayFrame] = []
    pending_dsn: dict[int, deque[int]] = defaultdict(deque)
    try:
        raw = path.read_bytes()
        encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8"
        lines = raw.decode(encoding, errors="ignore").splitlines()
    except OSError:
        return frames
    for line in lines:
        match = SERIAL_TX_RE.search(line)
        if match:
            payload = bytes.fromhex(match.group(3))
            if len(payload) >= 5:
                subcmd = int(match.group(2), 16)
                pending_dsn[subcmd].append(payload[4])
                frames.append(GatewayFrame(
                    timestamp=f"t={match.group(1)}ms",
                    from_gateway=False,
                    gateway_id=gateway_id,
                    type_code=0x0B0F,
                    payload=payload,
                    crc_ok=True,
                ))
            continue
        match = SERIAL_SHORT_ACK_RE.search(line)
        if match:
            frames.append(GatewayFrame(
                timestamp=f"t={match.group(1)}ms",
                from_gateway=True,
                gateway_id=gateway_id,
                type_code=0x0146,
                payload=bytes.fromhex(match.group(3)),
                crc_ok=True,
            ))
            continue
        match = SERIAL_ACTIVE_ACK_RE.search(line)
        if not match:
            continue
        request_subcmd = int(match.group(2), 16)
        dsn = pending_dsn[request_subcmd].popleft() if pending_dsn[request_subcmd] else 0
        credit = int(match.group(3), 16)
        response_subcmd = int(match.group(4), 16)
        body = bytes.fromhex(match.group(5)) if match.group(5) else b""
        payload = bytes((0x00, credit, 0x00, response_subcmd, dsn)) + body
        frames.append(GatewayFrame(
            timestamp=f"t={match.group(1)}ms",
            from_gateway=True,
            gateway_id=gateway_id,
            type_code=0x0B10,
            payload=payload,
            crc_ok=True,
        ))
    return frames


def gateway_alias(gateway_id: int, aliases: dict[int, str]) -> str:
    return aliases.get(gateway_id, f"GW#{gateway_id:04X}")


def node_alias(node_id: int, aliases: dict[int, str]) -> str:
    logical_id = node_id & 0x7FFF
    # Prefer the optimizer EUI whenever topology or a node-table response made
    # it available. RF short IDs may be reassigned between commissioning runs,
    # while the EUI is the stable identity needed for differential alignment.
    return aliases.get(logical_id, f"NODE#{logical_id}")


def normalize_node_body(subcmd: int, body: bytes, aliases: dict[int, str]) -> str:
    if subcmd in NODE_SUBCOMMANDS and len(body) >= 2:
        node_id = int.from_bytes(body[:2], "big")
        return f"{node_alias(node_id, aliases)}:{body[2:].hex().upper()}"
    if subcmd == 0x29 and body:
        count = body[0]
        entries = []
        pos = 1
        for _ in range(count):
            if pos + 10 > len(body):
                break
            eui = body[pos : pos + 8].hex().upper()
            raw_id = int.from_bytes(body[pos + 8 : pos + 10], "big")
            entries.append(f"{eui}:{node_alias(raw_id, aliases)}:pending={bool(raw_id & 0x8000)}")
            pos += 10
        return "|".join(entries) if entries else body.hex().upper()
    return body.hex().upper()


def normalized_frame_signature(
    frame: GatewayFrame,
    gateway_names: dict[int, str],
    node_names: dict[int, str],
) -> tuple[str, dict[str, Any]]:
    gateway = gateway_alias(frame.gateway_id, gateway_names)
    details: dict[str, Any] = {"payload_hex": frame.payload.hex().upper()}
    if frame.type_code in (0x0B0F, 0x0B10):
        command = decode_active_command(frame)
        if command:
            body = bytes.fromhex(command.raw_body_hex)
            details.update({
                "subcmd": command.subcmd,
                "dsn": command.dsn,
                "decoded": command.decoded,
            })
            prefix = "PV_REQ" if command.direction == "request" else "PV_RSP"
            credit = command.decoded.get("tx_buffers_free")
            body_text = normalize_node_body(command.subcmd, body, node_names)
            signature = f"{prefix}:{command.subcmd:02X}:{body_text}"
            if credit is not None:
                signature += f":credit={int(credit):02X}"
            return signature, details
    if frame.type_code == 0x0148:
        if len(frame.payload) == 5:
            details["cursor"] = int.from_bytes(frame.payload[2:4], "big")
            return f"RX_POLL:{frame.payload[:2].hex().upper()}:<CURSOR>:{frame.payload[4]:02X}", details
        return f"RX_BOOTSTRAP:{frame.payload.hex().upper()}", details
    if frame.type_code == 0x0149:
        parsed = parse_rx_status_and_blob(frame.payload)
        if parsed:
            status, blob = parsed
            details.update({"rx_status": status, "blob_hex": blob.hex().upper()})
            return f"RX_RESPONSE:status={status['status_type']:04X}:blob={bool(blob)}", details
    if frame.type_code == 0x003C and len(frame.payload) >= 14:
        eui = frame.payload[4:12].hex().upper()
        return f"MGMT:003C:TAP[{eui}]:ID", details
    if frame.type_code in (0x0039, 0x003B) and len(frame.payload) >= 10:
        eui = frame.payload[:8].hex().upper()
        return f"MGMT:{frame.type_code:04X}:TAP[{eui}]:ID", details
    return f"FRAME:{frame.type_code:04X}:{frame.payload.hex().upper()}", details


def expected_response_subcmd(request_subcmd: int) -> Optional[int]:
    return {
        0x06: 0x07,
        0x0D: 0x0E,
        0x13: 0x14,
        0x17: 0x18,
        0x22: 0x23,
        0x26: 0x27,
        0x29: 0x2A,
        0x2B: 0x2C,
        0x2D: 0x2F,
        0x2E: 0x2F,
        0x41: 0x41,
    }.get(request_subcmd)


def classify_pv_outcome(
    request_subcmd: int,
    response_subcmd: int,
    frame_type: int,
    credit: Optional[int],
    response_body: bytes,
) -> str:
    if frame_type == 0x0146:
        return "tap_local_ack"
    if request_subcmd in NODE_SUBCOMMANDS and response_subcmd == expected_response_subcmd(request_subcmd):
        suffix = f"_credit_{credit:02X}" if credit is not None else ""
        if response_body:
            return f"rf_response{suffix}"
        return f"tap_ack_empty_node_response{suffix}"
    return "tap_ack"


def build_trace_model(path: Path, *, release_vout: float = 5.0) -> TraceModel:
    decoded_frames = list(read_gateway_frames(path))
    discarded_crc_frame_count = sum(not frame.crc_ok for frame in decoded_frames)
    frames = [frame for frame in decoded_frames if frame.crc_ok]
    capture_mode = "gateway_frames"
    if not frames:
        frames = read_serial_diagnostic_frames(path)
        capture_mode = "serial_diagnostic_fallback"
    gateway_names, node_names = scan_identities(frames)
    frame_clock = FrameClock()
    events: list[TraceEvent] = []
    transactions: list[Transaction] = []
    pending: deque[int] = deque()
    unknown_rf: list[dict[str, Any]] = []
    first_release_ms: Optional[float] = None
    first_power_ms: Optional[float] = None
    first_rf_response_ms: Optional[float] = None

    for index, frame in enumerate(frames):
        time_ms = frame_time_ms(frame, frame_clock)
        direction = "tap" if frame.from_gateway else "host"
        signature, details = normalized_frame_signature(frame, gateway_names, node_names)
        event = TraceEvent(
            index=index,
            time_ms=time_ms,
            direction=direction,
            category="frame",
            signature=signature,
            gateway=gateway_alias(frame.gateway_id, gateway_names),
            frame_type=frame.type_code,
            details=details,
        )
        events.append(event)

        if not frame.from_gateway and frame.type_code in EXPECTED_TYPES:
            command = decode_active_command(frame)
            subcmd = command.subcmd if command else None
            dsn = command.dsn if command else None
            state_changing = request_is_state_changing(
                frame.type_code, frame.payload, subcmd
            )
            tx = Transaction(
                request_index=index,
                request_time_ms=time_ms,
                request_signature=signature,
                request_type=frame.type_code,
                gateway=event.gateway,
                expected_type=EXPECTED_TYPES[frame.type_code],
                dsn=dsn,
                subcmd=subcmd,
                state_changing=state_changing,
            )
            transactions.append(tx)
            pending.append(len(transactions) - 1)

        if frame.from_gateway:
            matched: Optional[int] = None
            response_command = decode_active_command(frame)
            # Retries are common in CCA traces. A response belongs to the most
            # recent compatible request; matching the oldest retry fabricates
            # multi-second latencies and leaves the actual request unmatched.
            pending_candidates = list(reversed(pending))
            for tx_index in pending_candidates:
                tx = transactions[tx_index]
                if tx.gateway != event.gateway and frame.gateway_id != 0:
                    continue
                if frame.type_code == 0x0146 and tx.request_type == 0x0B0F and tx.subcmd in NODE_SUBCOMMANDS:
                    matched = tx_index
                    tx.responses.append("tap_local_ack:0146")
                    tx.outcome = "tap_local_ack_waiting_rf"
                    break
                if frame.type_code != tx.expected_type:
                    continue
                if tx.request_type == 0x0B0F:
                    if response_command is None or response_command.dsn != tx.dsn:
                        continue
                    expected_subcmd = expected_response_subcmd(tx.subcmd) if tx.subcmd is not None else None
                    if expected_subcmd is not None and response_command.subcmd != expected_subcmd:
                        continue
                matched = tx_index
                break
            if matched is not None and frame.type_code != 0x0146:
                tx = transactions[matched]
                tx.responses.append(signature)
                tx.response_time_ms = time_ms
                tx.latency_ms = round(time_ms - tx.request_time_ms, 3)
                if tx.request_type == 0x0B0F and response_command is not None and tx.subcmd is not None:
                    credit = response_command.decoded.get("tx_buffers_free")
                    tx.outcome = classify_pv_outcome(
                        tx.subcmd, response_command.subcmd, frame.type_code,
                        int(credit) if credit is not None else None,
                        bytes.fromhex(response_command.raw_body_hex),
                    )
                    if tx.outcome.startswith("rf_response") and first_rf_response_ms is None:
                        first_rf_response_ms = time_ms
                else:
                    tx.outcome = "response"
                pending.remove(matched)

        if frame.type_code != 0x0149:
            continue
        parsed = parse_rx_status_and_blob(frame.payload)
        if not parsed:
            continue
        _, blob = parsed
        packets = split_pv_packets(blob)
        for packet_index, packet in enumerate(packets):
            ptype = int(packet["ptype"])
            packet_event = {
                "frame_index": index,
                "packet_index": packet_index,
                "time_ms": time_ms,
                "ptype": ptype,
                "node": node_alias(int(packet["node_id"]), node_names),
                "node_id": int(packet["node_id"]),
                "short_addr": int(packet["short_addr"]),
                "dsn": int(packet["dsn"]),
                "data_hex": packet["data"].hex().upper(),
            }
            category = "rf_packet"
            packet_signature = f"RF:{ptype:02X}:{packet_event['node']}"
            if ptype == 0x09:
                category = "topology"
                packet_signature = f"RF:TOPOLOGY:{packet_event['node']}"
                topology = decode_topology_report(packet["data"])
                if topology:
                    packet_event["topology"] = topology
            elif ptype == 0x27:
                category = "topology"
                packet_signature = f"RF:NODE_TABLE:{packet_event['node']}"
            elif ptype == 0x31:
                category = "power"
                packet_signature = f"RF:POWER:{packet_event['node']}"
                power = decode_power_report(packet["data"], iin_scale=0.0056)
                packet_event["power"] = power
                if first_power_ms is None:
                    first_power_ms = time_ms
                if power and power["vout_v"] >= release_vout and first_release_ms is None:
                    first_release_ms = time_ms
            elif ptype not in KNOWN_RF_PACKET_TYPES:
                category = "unknown_rf_packet"
                packet_signature = (
                    f"RF:UNKNOWN:{ptype:02X}:{packet_event['node']}:{packet_event['data_hex']}"
                )
                unknown_rf.append(packet_event)
            events.append(TraceEvent(
                index=index,
                time_ms=time_ms,
                direction="rf",
                category=category,
                signature=packet_signature,
                gateway=event.gateway,
                frame_type=0x0149,
                details=packet_event,
            ))
        consumed = sum(7 + int(packet["data_len"]) for packet in packets)
        if consumed < len(blob):
            remainder = blob[consumed:]
            malformed_event = {
                "frame_index": index,
                "packet_index": len(packets),
                "time_ms": time_ms,
                "ptype": int(remainder[0]) if remainder else 0,
                "node": "UNPARSED",
                "node_id": None,
                "short_addr": None,
                "dsn": None,
                "data_hex": remainder.hex().upper(),
                "malformed_remainder": True,
            }
            unknown_rf.append(malformed_event)
            events.append(TraceEvent(
                index=index,
                time_ms=time_ms,
                direction="rf",
                category="unknown_rf_packet",
                signature=f"RF:MALFORMED:{malformed_event['data_hex']}",
                gateway=event.gateway,
                frame_type=0x0149,
                details=malformed_event,
            ))

    capture_end_ms = max((event.time_ms for event in events if event.category == "frame"), default=0.0)
    for tx_index in pending:
        tx = transactions[tx_index]
        at_capture_edge = capture_end_ms - tx.request_time_ms <= CAPTURE_EDGE_PENDING_MS
        if at_capture_edge and not (
            capture_mode == "serial_diagnostic_fallback" and
            tx.outcome == "tap_local_ack_waiting_rf"
        ):
            tx.outcome = "capture_end_pending"
        elif tx.outcome == "tap_local_ack_waiting_rf":
            tx.outcome = "tap_local_ack_no_rf_response"
        else:
            tx.outcome = "no_response"

    return TraceModel(
        path=str(path),
        capture_mode=capture_mode,
        frame_count=len(frames),
        discarded_crc_frame_count=discarded_crc_frame_count,
        events=events,
        transactions=transactions,
        unknown_rf_packets=unknown_rf,
        first_release_ms=first_release_ms,
        first_power_ms=first_power_ms,
        first_rf_response_ms=first_rf_response_ms,
        gateway_aliases=gateway_names,
        node_aliases=node_names,
    )


def alignment_tokens(model: TraceModel) -> list[str]:
    timeline: list[tuple[int, int, int, str]] = []
    for tx in model.transactions:
        timeline.append((tx.request_index, 0, 0, tx.alignment_token))
    for event in model.events:
        if event.category not in ("rf_packet", "topology", "power", "unknown_rf_packet"):
            continue
        packet_index = int(event.details.get("packet_index", 0))
        timeline.append((event.index, 1, packet_index, event.signature))
    timeline.sort(key=lambda item: item[:3])
    return [item[3] for item in timeline]


def align_models(success: TraceModel, failure: TraceModel) -> dict[str, Any]:
    left = alignment_tokens(success)
    right = alignment_tokens(failure)
    matcher = difflib.SequenceMatcher(a=left, b=right, autojunk=False)
    blocks = []
    first_global_divergence = None
    matching_tokens = 0
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        block = {
            "tag": tag,
            "success_range": [i1, i2],
            "failure_range": [j1, j2],
            "success": left[i1:i2],
            "failure": right[j1:j2],
        }
        blocks.append(block)
        if tag == "equal":
            matching_tokens += i2 - i1
        elif first_global_divergence is None:
            first_global_divergence = block
    success_coverage = matching_tokens / len(left) if left else 1.0
    failure_coverage = matching_tokens / len(right) if right else 1.0
    return {
        "algorithm": "SequenceMatcher alignment",
        "success_tokens": len(left),
        "failure_tokens": len(right),
        "matching_tokens": matching_tokens,
        "ratio": matcher.ratio(),
        "success_coverage": round(success_coverage, 6),
        "failure_coverage": round(failure_coverage, 6),
        "low_coverage_threshold": ALIGNMENT_COVERAGE_WARNING_THRESHOLD,
        "low_coverage": min(success_coverage, failure_coverage) < ALIGNMENT_COVERAGE_WARNING_THRESHOLD,
        "first_global_sequence_divergence": first_global_divergence,
        "blocks": blocks,
    }


def exclusive_before_release(success: TraceModel, failure: TraceModel) -> list[dict[str, Any]]:
    cutoff = success.first_release_ms
    if cutoff is None:
        return []
    success_origin = min(
        (tx.request_time_ms for tx in success.transactions), default=0.0
    )
    failure_origin = min(
        (tx.request_time_ms for tx in failure.transactions), default=0.0
    )
    release_offset = max(0.0, cutoff - success_origin)
    failure_signatures = Counter(
        tx.request_signature
        for tx in failure.transactions
        if tx.request_time_ms - failure_origin <= release_offset
    )
    result = []
    seen = set()
    for tx in success.transactions:
        if tx.request_time_ms > cutoff or tx.request_signature in seen:
            continue
        seen.add(tx.request_signature)
        if failure_signatures[tx.request_signature] == 0:
            result.append({
                "signature": tx.request_signature,
                "time_ms": tx.request_time_ms,
                "outcome": tx.outcome,
                "state_changing": tx.state_changing,
            })
    return result


def first_common_outcome_divergence(success: TraceModel, failure: TraceModel) -> Optional[dict[str, Any]]:
    failure_by_signature: dict[str, deque[Transaction]] = defaultdict(deque)
    for tx in failure.transactions:
        if tx.outcome == "capture_end_pending":
            continue
        failure_by_signature[tx.request_signature].append(tx)
    for success_tx in success.transactions:
        if success_tx.outcome == "capture_end_pending":
            continue
        candidates = failure_by_signature.get(success_tx.request_signature)
        if not candidates:
            continue
        failure_tx = candidates.popleft()
        if success_tx.outcome == failure_tx.outcome:
            continue
        return {
            "signature": success_tx.request_signature,
            "success_time_ms": success_tx.request_time_ms,
            "success_outcome": success_tx.outcome,
            "success_responses": success_tx.responses,
            "failure_time_ms": failure_tx.request_time_ms,
            "failure_outcome": failure_tx.outcome,
            "failure_responses": failure_tx.responses,
        }
    return None


def summarize_model(model: TraceModel) -> dict[str, Any]:
    outcomes = Counter(tx.outcome for tx in model.transactions)
    capture_edge_pending = outcomes.get("capture_end_pending", 0)
    state_changes = [tx for tx in model.transactions if tx.state_changing]
    latencies = [tx.latency_ms for tx in model.transactions if tx.latency_ms is not None]
    frame_events = [event for event in model.events if event.category == "frame"]
    duration_ms = None
    if frame_events:
        duration_ms = round(
            max(event.time_ms for event in frame_events) - min(event.time_ms for event in frame_events),
            3,
        )
    completed_transactions = [
        tx for tx in model.transactions if tx.outcome != "capture_end_pending"
    ]
    response_observed = sum(bool(tx.responses) for tx in completed_transactions)
    response_coverage = (
        response_observed / len(completed_transactions)
        if completed_transactions else 1.0
    )
    rf_event_counts = Counter(
        event.category for event in model.events
        if event.category in ("rf_packet", "topology", "power", "unknown_rf_packet")
    )
    completeness_warnings = [
        "Capture completeness cannot be proven from the trace alone; coverage values are observability proxies."
    ]
    if model.capture_mode == "serial_diagnostic_fallback":
        completeness_warnings.append(
            "Serial diagnostic fallback reconstructs selected request/response events and is not a complete bus capture."
        )
    if model.discarded_crc_frame_count:
        completeness_warnings.append(
            f"{model.discarded_crc_frame_count} decoded frame(s) were discarded because CRC validation failed."
        )
    if capture_edge_pending:
        completeness_warnings.append(
            f"{capture_edge_pending} request(s) at the capture boundary remain pending and are excluded from outcome divergence."
        )
    return {
        "path": model.path,
        "capture_completeness": {
            "verifiable": False,
            "capture_mode": model.capture_mode,
            "duration_ms": duration_ms,
            "valid_frames": model.frame_count,
            "discarded_crc_frames": model.discarded_crc_frame_count,
            "transactions_with_observed_response": response_observed,
            "capture_edge_pending_transactions": capture_edge_pending,
            "transaction_response_coverage": round(response_coverage, 6),
            "warnings": completeness_warnings,
        },
        "frames": model.frame_count,
        "transactions": len(model.transactions),
        "alignment_events": len(alignment_tokens(model)),
        "rf_event_counts": dict(sorted(rf_event_counts.items())),
        "outcomes": dict(sorted(outcomes.items())),
        "state_changing_transactions": len(state_changes),
        "first_power_ms": model.first_power_ms,
        "first_release_ms": model.first_release_ms,
        "first_rf_response_ms": model.first_rf_response_ms,
        "response_latency_ms": {
            "min": min(latencies) if latencies else None,
            "max": max(latencies) if latencies else None,
            "average": round(sum(latencies) / len(latencies), 3) if latencies else None,
        },
        "unknown_rf_packet_count": len(model.unknown_rf_packets),
        "unknown_rf_packet_types": dict(sorted(Counter(
            f"0x{item['ptype']:02X}" for item in model.unknown_rf_packets
        ).items())),
    }


def larger_to_smaller_ratio(left: int, right: int) -> Optional[float]:
    smaller = min(left, right)
    if smaller == 0:
        return None
    return round(max(left, right) / smaller, 3)


def comparison_quality(
    success_summary: dict[str, Any],
    failure_summary: dict[str, Any],
    alignment: dict[str, Any],
) -> dict[str, Any]:
    ratios = {
        "frames_larger_to_smaller": larger_to_smaller_ratio(
            int(success_summary["frames"]), int(failure_summary["frames"])
        ),
        "transactions_larger_to_smaller": larger_to_smaller_ratio(
            int(success_summary["transactions"]), int(failure_summary["transactions"])
        ),
        "alignment_events_larger_to_smaller": larger_to_smaller_ratio(
            int(success_summary["alignment_events"]), int(failure_summary["alignment_events"])
        ),
    }
    compared_counts = (
        (int(success_summary[key]), int(failure_summary[key]))
        for key in ("frames", "transactions", "alignment_events")
    )
    one_side_empty = any((left == 0) != (right == 0) for left, right in compared_counts)
    material_asymmetry = one_side_empty or any(
        ratio is not None and ratio >= CAPTURE_ASYMMETRY_WARNING_THRESHOLD
        for ratio in ratios.values()
    )
    warnings = []
    if material_asymmetry:
        warnings.append(
            "Capture sizes are materially asymmetric; global sequence divergence may reflect missing coverage."
        )
    if alignment["low_coverage"]:
        warnings.append(
            "Alignment coverage is low; do not interpret the first global divergence as a causal protocol boundary."
        )
    success_mode = success_summary["capture_completeness"]["capture_mode"]
    failure_mode = failure_summary["capture_completeness"]["capture_mode"]
    if success_mode != failure_mode:
        warnings.append(
            f"Capture modes differ ({success_mode} vs {failure_mode}); event visibility is not equivalent."
        )
    return {
        "capture_asymmetry": {
            "warning_threshold_larger_to_smaller": CAPTURE_ASYMMETRY_WARNING_THRESHOLD,
            "one_side_empty": one_side_empty,
            "materially_asymmetric": material_asymmetry,
            **ratios,
        },
        "alignment_coverage": {
            "matching_tokens": alignment["matching_tokens"],
            "success_coverage": alignment["success_coverage"],
            "failure_coverage": alignment["failure_coverage"],
            "low_coverage_threshold": alignment["low_coverage_threshold"],
            "low_coverage": alignment["low_coverage"],
        },
        "warnings": warnings,
    }


def compare_traces(success_path: Path, failure_path: Path, *, release_vout: float = 5.0) -> dict[str, Any]:
    success = build_trace_model(success_path, release_vout=release_vout)
    failure = build_trace_model(failure_path, release_vout=release_vout)
    alignment = align_models(success, failure)
    success_summary = summarize_model(success)
    failure_summary = summarize_model(failure)
    return {
        "schema_version": 2,
        "success": success_summary,
        "failure": failure_summary,
        "comparison_quality": comparison_quality(success_summary, failure_summary, alignment),
        "alignment": alignment,
        "first_common_request_outcome_divergence": first_common_outcome_divergence(success, failure),
        "exclusive_before_first_release": exclusive_before_release(success, failure),
        "unknown_rf_packets": {
            "success": success.unknown_rf_packets,
            "failure": failure.unknown_rf_packets,
        },
        "transactions": {
            "success": [asdict(tx) for tx in success.transactions],
            "failure": [asdict(tx) for tx in failure.transactions],
        },
    }


def render_markdown(report: dict[str, Any]) -> str:
    success = report["success"]
    failure = report["failure"]
    alignment = report["alignment"]
    quality = report["comparison_quality"]
    divergence = alignment.get("first_global_sequence_divergence")
    outcome_divergence = report.get("first_common_request_outcome_divergence")
    success_completeness = success["capture_completeness"]
    failure_completeness = failure["capture_completeness"]
    lines = [
        "# Differential TAP trace report",
        "",
        "| Metric | Successful trace | Failed trace |",
        "|---|---:|---:|",
        f"| Frames | {success['frames']} | {failure['frames']} |",
        f"| Transactions | {success['transactions']} | {failure['transactions']} |",
        f"| Chronological alignment events | {success['alignment_events']} | {failure['alignment_events']} |",
        f"| Capture mode | {success_completeness['capture_mode']} | {failure_completeness['capture_mode']} |",
        f"| Capture duration | {success_completeness['duration_ms']} ms | {failure_completeness['duration_ms']} ms |",
        f"| Transaction response coverage | {success_completeness['transaction_response_coverage']:.1%} | {failure_completeness['transaction_response_coverage']:.1%} |",
        f"| First RF response | {success['first_rf_response_ms']} ms | {failure['first_rf_response_ms']} ms |",
        f"| First 0x31 | {success['first_power_ms']} ms | {failure['first_power_ms']} ms |",
        f"| First released Vout | {success['first_release_ms']} ms | {failure['first_release_ms']} ms |",
        f"| State-changing transactions | {success['state_changing_transactions']} | {failure['state_changing_transactions']} |",
        "",
        "## Comparison quality",
        "",
        f"- SequenceMatcher ratio: `{alignment['ratio']:.3f}`",
        f"- Successful-trace alignment coverage: `{alignment['success_coverage']:.1%}`",
        f"- Failed-trace alignment coverage: `{alignment['failure_coverage']:.1%}`",
        f"- Material capture asymmetry: `{quality['capture_asymmetry']['materially_asymmetric']}`",
    ]
    if quality["warnings"]:
        lines.extend(f"- Warning: {warning}" for warning in quality["warnings"])
    else:
        lines.append("- No comparison-quality warning was triggered.")
    lines.extend([
        "",
        "Capture completeness is not directly verifiable; the table reports observable coverage proxies.",
        "",
        "## First global sequence-alignment divergence",
        "",
        "This is the first edit operation in the complete chronological token alignment; it is not a paired request-outcome comparison.",
        "",
    ])
    if divergence:
        lines.extend([
            f"- Alignment operation: `{divergence['tag']}`",
            f"- Successful: `{divergence['success'][:3]}`",
            f"- Failed: `{divergence['failure'][:3]}`",
        ])
    else:
        lines.append("No sequence divergence found.")
    lines.extend([
        "",
        "## First common request-outcome divergence",
        "",
        "This compares occurrences of the same normalized request signature and then checks their response outcomes.",
        "",
    ])
    if outcome_divergence:
        lines.extend([
            f"- Request: `{outcome_divergence['signature']}`",
            f"- Successful: `{outcome_divergence['success_outcome']}` at `{outcome_divergence['success_time_ms']} ms`",
            f"- Failed: `{outcome_divergence['failure_outcome']}` at `{outcome_divergence['failure_time_ms']} ms`",
        ])
    else:
        lines.append("No common request with a different response outcome was found.")
    lines.extend(["", "## Success-only commands before first released Vout", ""])
    exclusive = report["exclusive_before_first_release"]
    if exclusive:
        lines.extend(
            f"- `{item['time_ms']:.3f} ms` `{item['signature']}` -> `{item['outcome']}`"
            for item in exclusive
        )
    else:
        lines.append("None, or no released Vout was present in the successful trace.")
    lines.extend(["", "## Unknown RF packet types", ""])
    lines.append(f"- Successful: `{success['unknown_rf_packet_types']}`")
    lines.append(f"- Failed: `{failure['unknown_rf_packet_types']}`")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("success", type=Path)
    parser.add_argument("failure", type=Path)
    parser.add_argument("--release-vout", type=float, default=5.0)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--output-markdown", type=Path)
    args = parser.parse_args()
    report = compare_traces(args.success, args.failure, release_vout=args.release_vout)
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)
    if args.output_markdown:
        args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
        args.output_markdown.write_text(render_markdown(report), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
