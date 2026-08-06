#!/usr/bin/env python3
"""Build and safely execute response-aware TAP trace replay plans."""

from __future__ import annotations

import argparse
import copy
import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable, Optional

from tigo_tap_decoder_v2 import decode_active_command, read_gateway_frames
from tigo_trace_diff import (
    EXPECTED_TYPES,
    READ_ONLY_TYPES,
    STATE_CHANGING_TYPES,
    build_trace_model,
    frame_time_ms,
    gateway_alias,
    scan_identities,
)


READ_ONLY_PV_SUBCOMMANDS = {0x0D, 0x26, 0x2E}
ACTIVE_RF_SUBCOMMANDS = {0x06, 0x17}
STATE_CHANGING_PV_SUBCOMMANDS = {0x13, 0x22, 0x29, 0x2B, 0x2D, 0x41}


def pv_command_risk(subcmd: int) -> str:
    if subcmd in READ_ONLY_PV_SUBCOMMANDS:
        return "read_only"
    if subcmd in ACTIVE_RF_SUBCOMMANDS:
        return "active_rf"
    if subcmd in STATE_CHANGING_PV_SUBCOMMANDS:
        return "state_changing"
    return "unknown"


def frame_risk(type_code: int, payload: bytes = b"") -> str:
    if type_code == 0x0B00 and payload in (b"\x00", b"\x01"):
        return "state_changing"
    if type_code in STATE_CHANGING_TYPES:
        return "state_changing"
    if type_code in READ_ONLY_TYPES:
        return "read_only"
    return "unknown"


def expected_response_subcmd(subcmd: int) -> Optional[int]:
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
    }.get(subcmd)


def operational_target(alias: str, source_gateway_id: Optional[int] = None) -> str:
    # Identity discovery intentionally aliases every address used by the same
    # TAP to its EUI. Preserve protocol-significant temporary IDs before that
    # alias collapses 0x1209 and 0x120A into the same logical device.
    if source_gateway_id == 0x0000 or alias == "BROADCAST":
        return "0x0000"
    if source_gateway_id == 0x1235 or alias == "ENUM":
        return "0x1235"
    if source_gateway_id == 0x120A or alias == "TEMP_120A":
        return "0x120A"
    return "current"


def build_replay_plan(path: Path, *, through_first_release: bool = True) -> dict[str, Any]:
    frames = [frame for frame in read_gateway_frames(path) if frame.crc_ok]
    if not frames:
        raise ValueError("replay plans require a raw or predecoded bidirectional trace")
    gateway_names, node_names = scan_identities(frames)
    model = build_trace_model(path)
    outcome_by_request = {tx.request_index: tx for tx in model.transactions}
    cutoff = model.first_release_ms if through_first_release else None
    first_iso = [None]
    selected: list[dict[str, Any]] = []
    previous_ms: Optional[float] = None
    first_request_ms: Optional[float] = None
    block = 0

    for index, frame in enumerate(frames):
        if frame.from_gateway:
            continue
        time_ms = frame_time_ms(frame, first_iso)
        if cutoff is not None and time_ms > cutoff:
            break
        if (
            frame.type_code == 0x0148
            and len(frame.payload) == 5
            and frame.payload[:2] == b"\x00\x01"
            and frame.payload[4] == 0x04
        ):
            continue
        if frame.type_code not in EXPECTED_TYPES:
            continue
        delay_ms = 0.0 if previous_ms is None else max(0.0, time_ms - previous_ms)
        if first_request_ms is None:
            first_request_ms = time_ms
        if previous_ms is not None and delay_ms >= 2000.0:
            block += 1
        previous_ms = time_ms
        alias = gateway_alias(frame.gateway_id, gateway_names)
        transaction = outcome_by_request.get(index)
        base = {
            "index": len(selected),
            "source_frame_index": index,
            "block": block,
            "offset_ms": time_ms,
            "relative_offset_ms": round(time_ms - first_request_ms, 3),
            "delay_ms": round(delay_ms, 3),
            "target": operational_target(alias, frame.gateway_id),
            "source_target_alias": alias,
            "risk": frame_risk(frame.type_code, frame.payload),
            "state_changing": frame_risk(frame.type_code, frame.payload) == "state_changing",
            "expected_outcome": transaction.outcome if transaction else "response",
            "expected_latency_ms": transaction.latency_ms if transaction else None,
        }
        if frame.type_code == 0x0B0F:
            command = decode_active_command(frame)
            if command is None:
                continue
            body = bytes.fromhex(command.raw_body_hex)
            risk = pv_command_risk(command.subcmd)
            base.update({
                "action": "pv_subcommand",
                "subcmd": command.subcmd,
                # DSN is deliberately absent. The live firmware allocates it.
                "body_hex": command.raw_body_hex,
                "expected_response_subcmd": expected_response_subcmd(command.subcmd),
                "risk": risk,
                "state_changing": risk == "state_changing",
            })
            if command.subcmd in (0x06, 0x13, 0x17) and len(body) >= 2:
                source_node_id = int.from_bytes(body[:2], "big") & 0x7FFF
                source_alias = node_names.get(source_node_id, "")
                node_eui = (
                    source_alias[5:-1]
                    if source_alias.startswith("NODE[") and source_alias.endswith("]")
                    else ""
                )
                base["rf_node"] = {
                    "source_node_id": source_node_id,
                    "eui64": node_eui,
                }
                base["body_suffix_hex"] = body[2:].hex().upper()
            elif command.subcmd == 0x29 and body:
                count = body[0]
                entries = []
                pos = 1
                for _ in range(count):
                    if pos + 10 > len(body):
                        break
                    entries.append({
                        "eui64": body[pos : pos + 8].hex().upper(),
                        "source_raw_node_id": int.from_bytes(body[pos + 8 : pos + 10], "big"),
                    })
                    pos += 10
                if len(entries) == count:
                    base["node_table_entries"] = entries
                    base["body_trailing_hex"] = body[pos:].hex().upper()
        elif frame.type_code == 0x0148:
            base.update({
                "action": "receive_bootstrap",
                "payload_hex": frame.payload.hex().upper(),
                "expected_type": 0x0149,
            })
        else:
            base.update({
                "action": "simple_frame",
                "type": frame.type_code,
                "payload_hex": frame.payload.hex().upper(),
                "expected_type": EXPECTED_TYPES[frame.type_code],
            })
            if frame.type_code == 0x003C and len(frame.payload) >= 14:
                desired_id = int.from_bytes(frame.payload[12:14], "big")
                base["address_assignment"] = {
                    "source_eui64": frame.payload[4:12].hex().upper(),
                    "desired_target": operational_target(
                        gateway_alias(desired_id, gateway_names), desired_id
                    ),
                    "trailing_hex": frame.payload[14:].hex().upper(),
                }
        selected.append(base)

    tap_euis = sorted({
        alias[4:-1]
        for alias in gateway_names.values()
        if alias.startswith("TAP[") and alias.endswith("]")
    })
    return {
        "schema_version": 1,
        "source_trace": str(path),
        "through_first_release": through_first_release,
        "source_first_request_ms": first_request_ms,
        "source_first_release_ms": model.first_release_ms,
        "source_tap_euis": tap_euis,
        "dynamic_fields": [
            "gateway_id", "tap_eui64", "pv_dsn", "packet_cursor",
            "rf_short_address", "timestamp"
        ],
        "success_criterion": {
            "fresh_power": True,
            "electrical_release_observed": True,
        },
        "steps": selected,
    }


class ReplayClient:
    def __init__(self, base_url: str, timeout: float = 5.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request_json(
        self, path: str, *, method: str = "GET", data: Optional[bytes] = None
    ) -> dict[str, Any]:
        attempts = 3 if method == "GET" and data is None else 1
        for attempt in range(attempts):
            request = urllib.request.Request(self.base_url + path, method=method, data=data)
            if data is not None:
                request.add_header("Content-Type", "application/x-www-form-urlencoded")
            try:
                with urllib.request.urlopen(request, timeout=self.timeout) as response:
                    return json.load(response)
            except (OSError, urllib.error.URLError):
                if attempt + 1 >= attempts:
                    raise
                time.sleep(0.15 * (attempt + 1))
        raise RuntimeError("unreachable HTTP retry state")

    def post_form(self, path: str, values: dict[str, Any]) -> dict[str, Any]:
        encoded = urllib.parse.urlencode(values).encode("ascii")
        return self.request_json(path, method="POST", data=encoded)

    def status(self) -> dict[str, Any]:
        return self.request_json("/api/status")

    def journal(self) -> dict[str, Any]:
        return self.request_json("/api/boot-journal")

    def node_map(self) -> dict[str, Any]:
        return self.request_json("/api/node-map")

    def replay_session(self, mode: str) -> dict[str, Any]:
        query = urllib.parse.urlencode({"mode": mode, "confirm": "TRACE_REPLAY"})
        return self.request_json("/api/command/replay-session?" + query, method="POST")

    def trace_replay(self) -> dict[str, Any]:
        return self.request_json("/api/trace-replay")

    def trace_replay_result(self, index: int) -> dict[str, Any]:
        query = urllib.parse.urlencode({"index": index})
        return self.request_json("/api/trace-replay/result?" + query)

    def trace_replay_plan(self, mode: str, **values: Any) -> dict[str, Any]:
        form = {"mode": mode, "confirm": "TRACE_REPLAY", **values}
        return self.post_form("/api/command/trace-replay-plan", form)

    def wait_command(self, timeout: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.status()
            if not status.get("command_busy", False):
                return status
            time.sleep(0.05)
        raise TimeoutError("TAP command did not complete")


class ElectricalMqttProbe:
    """Observe inverter voltage/power independently from the TAP receive queue."""

    def __init__(
        self,
        broker: str,
        port: int,
        power_topic: str,
        voltage_topic: str,
        min_power_w: float,
        min_voltage_v: float,
    ):
        try:
            import paho.mqtt.client as mqtt
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise RuntimeError("paho-mqtt is required for electrical verification") from exc
        self.power_topic = power_topic
        self.voltage_topic = voltage_topic
        self.min_power_w = min_power_w
        self.min_voltage_v = min_voltage_v
        self.values: dict[str, tuple[float, float]] = {}
        self.ready = threading.Event()
        self.client = mqtt.Client()

        def on_connect(client, _userdata, _flags, rc):
            if rc != 0:
                return
            client.subscribe([(power_topic, 0), (voltage_topic, 0)])

        def on_message(_client, _userdata, message):
            try:
                value = float(message.payload.decode("ascii", errors="strict"))
            except (UnicodeDecodeError, ValueError):
                return
            self.values[message.topic] = (value, time.monotonic())
            if power_topic in self.values and voltage_topic in self.values:
                self.ready.set()

        self.client.on_connect = on_connect
        self.client.on_message = on_message
        self.client.connect(broker, port, 30)
        self.client.loop_start()
        if not self.ready.wait(8.0):
            self.close()
            raise RuntimeError("no fresh inverter MQTT values received")

    def snapshot(self) -> dict[str, Any]:
        now = time.monotonic()
        power, power_at = self.values.get(self.power_topic, (0.0, 0.0))
        voltage, voltage_at = self.values.get(self.voltage_topic, (0.0, 0.0))
        fresh = now - power_at <= 15.0 and now - voltage_at <= 15.0
        return {
            "power_w": power,
            "voltage_v": voltage,
            "fresh": fresh,
            "released": fresh and power >= self.min_power_w and voltage >= self.min_voltage_v,
        }

    def close(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()


def resolve_target(step: dict[str, Any], status: dict[str, Any]) -> int:
    target = str(step["target"])
    if target == "current":
        return int(str(status["gateway_id_hex"]), 16)
    return int(target, 0)


def live_nodes_by_eui(client: ReplayClient) -> dict[str, dict[str, Any]]:
    return {
        str(node.get("long_addr", "")).upper(): node
        for node in client.node_map().get("nodes", [])
        if node.get("long_addr")
    }


def resolve_pv_body(client: ReplayClient, step: dict[str, Any]) -> str:
    node_ref = step.get("rf_node")
    if node_ref:
        eui = str(node_ref.get("eui64", "")).upper()
        if not eui:
            raise RuntimeError(
                f"step {step['index']} has no EUI mapping for source node "
                f"{node_ref.get('source_node_id')}"
            )
        live = live_nodes_by_eui(client).get(eui)
        if live is None:
            raise RuntimeError(f"step {step['index']} optimizer {eui} is absent from the live node table")
        node_id = int(live["node_id"]) & 0x7FFF
        return f"{node_id:04X}" + str(step.get("body_suffix_hex", ""))
    entries = step.get("node_table_entries")
    if entries is not None:
        live_nodes = live_nodes_by_eui(client)
        encoded = bytearray((len(entries),))
        for entry in entries:
            eui = str(entry["eui64"]).upper()
            live = live_nodes.get(eui)
            if live is None:
                raise RuntimeError(f"step {step['index']} optimizer {eui} is absent from the live node table")
            encoded.extend(bytes.fromhex(eui))
            encoded.extend(int(str(live["raw_node_id_hex"]), 16).to_bytes(2, "big"))
        encoded.extend(bytes.fromhex(str(step.get("body_trailing_hex", ""))))
        return encoded.hex().upper()
    return str(step.get("body_hex", ""))


def resolve_simple_payload(step: dict[str, Any], status: dict[str, Any]) -> str:
    assignment = step.get("address_assignment")
    if assignment is None:
        return str(step.get("payload_hex", ""))
    live_eui = str(status.get("gateway_long_addr", "")).upper()
    if len(live_eui) != 16:
        raise RuntimeError("live TAP EUI-64 is unavailable for address assignment")
    desired = str(assignment["desired_target"])
    desired_id = int(status["gateway_id_hex"], 16) if desired == "current" else int(desired, 0)
    return "37249266" + live_eui + f"{desired_id:04X}" + str(assignment.get("trailing_hex", ""))


def firmware_replay_outcome(step: dict[str, Any]) -> tuple[str, int]:
    expected = str(step.get("expected_outcome", "response"))
    if expected.startswith("rf_response_credit_"):
        return "rf_response", int(expected.rsplit("_", 1)[1], 16)
    if expected.startswith("tap_ack_empty_node_response"):
        credit = int(expected.rsplit("_", 1)[1], 16) if "_credit_" in expected else 0xFF
        return "tap_ack", credit
    if expected in {"tap_local_ack_no_rf_response", "tap_local_ack_waiting_rf"}:
        return "local_tap_ack", 0xFF
    if expected == "tap_ack":
        return "tap_ack", 0xFF
    if expected == "no_response":
        return "no_response", 0xFF
    if expected == "response":
        return "response", 0xFF
    raise ValueError(f"unsupported response outcome for ESP scheduler: {expected}")


def encode_firmware_replay_step(
    client: ReplayClient,
    step: dict[str, Any],
    status: dict[str, Any],
    *,
    timing_scale: float,
    upload_index: int,
) -> dict[str, Any]:
    action = str(step["action"])
    if action == "pv_subcommand":
        type_code = int(step["subcmd"])
        expected_type = 0x0B10
        payload_hex = resolve_pv_body(client, step)
        target = 0xFFFF
    elif action == "receive_bootstrap":
        type_code = 0x0148
        expected_type = 0x0149
        payload_hex = str(step.get("payload_hex", ""))
        target = 0xFFFF
    elif action == "simple_frame":
        type_code = int(step["type"])
        expected_type = int(step["expected_type"])
        payload_hex = resolve_simple_payload(step, status)
        target = 0xFFFF if str(step["target"]) == "current" else resolve_target(step, status)
    else:
        raise ValueError(f"unknown replay action: {action}")
    outcome, expected_credit = firmware_replay_outcome(step)
    rf_node = 0
    if action == "pv_subcommand" and step.get("rf_node") and len(payload_hex) >= 4:
        rf_node = int(payload_hex[:4], 16) & 0x7FFF
    return {
        "index": upload_index,
        "offset_ms": int(round(float(step.get("relative_offset_ms", 0.0)) * timing_scale)),
        "action": action,
        "target": f"0x{target:04X}",
        "type": f"0x{type_code:04X}",
        "payload_hex": payload_hex,
        "expected_type": f"0x{expected_type:04X}",
        "outcome": outcome,
        "expected_subcmd": f"0x{int(step.get('expected_response_subcmd') or 0):02X}",
        "expected_credit": f"0x{expected_credit:02X}",
        "rf_node": rf_node,
        "block": int(step.get("block", 0)),
    }


def classify_live_outcome(step: dict[str, Any], status: dict[str, Any]) -> str:
    command_state = str(status.get("command_state", "")).lower()
    if "timeout" in command_state or "failed" in command_state:
        if step["action"] == "pv_subcommand" and step.get("expected_outcome") == "tap_local_ack_no_rf_response":
            return "tap_local_ack_no_rf_response"
        return "no_response"
    if step["action"] == "pv_subcommand":
        response_subcmd = str(status.get("last_pv_ack_rsp_subcmd_hex", ""))
        credit_hex = str(status.get("last_pv_ack_status_hex", ""))
        if step.get("subcmd") in (0x06, 0x13, 0x17) and response_subcmd:
            credit = int(credit_hex, 16) if credit_hex else 0
            if str(status.get("last_pv_ack_body_hex", "")):
                return f"rf_response_credit_{credit:02X}"
            return f"tap_ack_empty_node_response_credit_{credit:02X}"
        return "tap_ack"
    return "response"


def outcome_matches(expected: str, actual: str) -> bool:
    if expected == actual:
        return True
    if expected == "tap_local_ack_waiting_rf" and actual == "tap_local_ack_no_rf_response":
        return True
    return False


def execute_step(
    client: ReplayClient, step: dict[str, Any], status: dict[str, Any]
) -> tuple[dict[str, Any], dict[str, Any]]:
    action = step["action"]
    expected_gateway = resolve_target(step, status)
    if action == "pv_subcommand":
        query = urllib.parse.urlencode({
            "subcmd": f"0x{int(step['subcmd']):02X}",
            "body_hex": resolve_pv_body(client, step),
        })
        result = client.request_json("/api/command/pv-subcmd?" + query)
        expected_command_name = "pv_subcmd"
        expected_type = 0x0B10
    elif action == "simple_frame":
        fire_and_forget = step.get("expected_outcome") == "no_response"
        type_code = int(step["type"])
        request_method = "GET"
        query_args = {
            "gatewayId": f"0x{expected_gateway:04X}",
            "type": f"0x{type_code:04X}",
            "payload_hex": resolve_simple_payload(step, status),
            "expectedType": f"0x{int(step['expected_type']):04X}",
            "expectedGatewayId": f"0x{expected_gateway:04X}",
            "wait": "false" if fire_and_forget else "true",
            "timeoutMs": 3000,
        }
        if type_code in (0x0014, 0x003C):
            raise RuntimeError(
                f"legacy HTTP transport cannot safely replay 0x{type_code:04X} address changes; "
                "use --transport esp"
            )
        if type_code in (0x0010, 0x0012):
            query_args["confirm"] = "ADDRESS_CHANGE"
            request_method = "POST"
        elif type_code == 0x0B00:
            payload = bytes.fromhex(str(query_args["payload_hex"]))
            if payload == b"\x01":
                query_args["confirm"] = "RSD_RUN"
            elif payload == b"\x00":
                query_args["confirm"] = "RSD_STOP"
            else:
                raise RuntimeError("unsupported RSD control payload")
            request_method = "POST"
        elif str(step.get("risk")) == "unknown":
            query_args["confirm"] = "UNSAFE_RAW_FRAME"
            request_method = "POST"
        query = urllib.parse.urlencode(query_args)
        result = client.request_json(
            "/api/command/simple-frame?" + query,
            method=request_method,
        )
        expected_command_name = "simple_frame"
        expected_type = int(step["expected_type"])
    elif action == "receive_bootstrap":
        payload = str(step["payload_hex"]).upper()
        query = urllib.parse.urlencode({"payload_hex": payload, "confirm": "RECEIVE_BOOTSTRAP"})
        result = client.request_json("/api/command/receive-bootstrap?" + query, method="POST")
        expected_command_name = "boot_rx_seed"
        expected_type = 0x0149
    else:
        raise ValueError(f"unknown replay action: {action}")
    if not result.get("ok", False):
        raise RuntimeError(f"step rejected: {result}")
    if action == "simple_frame" and step.get("expected_outcome") == "no_response":
        return client.status(), {
            "expected_command_name": "",
            "expected_type": expected_type,
            "expected_gateway": expected_gateway,
            "fire_and_forget": True,
        }
    expected_latency = float(step.get("expected_latency_ms") or 0.0) / 1000.0
    after = client.wait_command(max(5.0, expected_latency + 3.0))
    return after, {
        "expected_command_name": expected_command_name,
        "expected_type": expected_type,
        "expected_gateway": expected_gateway,
    }


def validate_live_response(
    step: dict[str, Any], status: dict[str, Any], metadata: dict[str, Any], actual_outcome: str
) -> None:
    actual_command = str(status.get("command_name", ""))
    if metadata.get("fire_and_forget"):
        return
    if actual_command != metadata["expected_command_name"]:
        raise RuntimeError(
            f"step {step['index']} completed as {actual_command!r}, "
            f"expected {metadata['expected_command_name']!r}"
        )
    response_type = int(str(status.get("last_command_response_type_hex", "0")), 16)
    response_gateway = int(str(status.get("last_command_response_gateway_hex", "0")), 16)
    if actual_outcome == "no_response":
        if response_type != 0:
            raise RuntimeError(
                f"step {step['index']} expected no response but recorded type 0x{response_type:04X}"
            )
        return
    if actual_outcome == "tap_local_ack_no_rf_response":
        if response_type not in (0, 0x0146):
            raise RuntimeError(
                f"step {step['index']} expected only local ACK but recorded type 0x{response_type:04X}"
            )
        return
    if response_type != int(metadata["expected_type"]):
        raise RuntimeError(
            f"step {step['index']} response type 0x{response_type:04X}, "
            f"expected 0x{int(metadata['expected_type']):04X}"
        )
    if response_gateway != int(metadata["expected_gateway"]):
        raise RuntimeError(
            f"step {step['index']} response gateway 0x{response_gateway:04X}, "
            f"expected 0x{int(metadata['expected_gateway']):04X}"
        )
    if step["action"] == "pv_subcommand":
        request_hex = str(status.get("last_pv_request_hex", ""))
        if len(request_hex) < 10:
            raise RuntimeError(f"step {step['index']} has no captured live PV request DSN")
        request_dsn = int(request_hex[8:10], 16)
        response_dsn = int(str(status.get("last_command_response_dsn_hex", "0")), 16)
        if request_dsn != response_dsn:
            raise RuntimeError(
                f"step {step['index']} PV DSN mismatch request=0x{request_dsn:02X} "
                f"response=0x{response_dsn:02X}"
            )


def execute_plan(
    plan: dict[str, Any],
    *,
    base_url: str,
    timing_scale: float,
    allow_state_changing: bool,
    allow_active_rf: bool = False,
    electrical_probe: Optional[ElectricalMqttProbe] = None,
    accept_maintained_release: bool = False,
) -> dict[str, Any]:
    client = ReplayClient(base_url)
    initial = client.status()
    journal = client.journal()
    source_euis = set(plan.get("source_tap_euis", []))
    live_eui = str(initial.get("gateway_long_addr", "")).upper()
    if source_euis and live_eui not in source_euis:
        raise RuntimeError(f"TAP identity mismatch: live={live_eui}, source={sorted(source_euis)}")
    for step in plan["steps"]:
        if step.get("state_changing") and not allow_state_changing:
            raise PermissionError(
                f"step {step['index']} is state-changing; pass --allow-state-changing explicitly"
            )
        if step.get("risk") == "active_rf" and not (allow_active_rf or allow_state_changing):
            raise PermissionError(
                f"step {step['index']} actively addresses an optimizer; pass --allow-active-rf explicitly"
            )
        if step.get("risk", "unknown") == "unknown" and not allow_state_changing:
            raise PermissionError(
                f"step {step['index']} has unknown protocol semantics; "
                "pass --allow-state-changing only after manual review"
            )
    baseline = {
        "gateway_id_hex": initial.get("gateway_id_hex"),
        "gateway_long_addr": live_eui,
        "radio_profile_fingerprint_fnv1a32": initial.get("radio_profile_fingerprint_fnv1a32"),
        "node_table_hash_fnv1a32": initial.get("node_table_hash_fnv1a32"),
    }
    electrical_before = electrical_probe.snapshot() if electrical_probe else None
    results = []
    stopped_after_step: Optional[int] = None
    session = client.replay_session("start")
    if not session.get("ok", False):
        raise RuntimeError(f"exclusive replay session rejected: {session}")
    first_source_offset = float(plan["steps"][0].get("offset_ms", 0.0)) if plan["steps"] else 0.0
    replay_started = time.monotonic()
    try:
        for step in plan["steps"]:
            relative_ms = float(step.get(
                "relative_offset_ms",
                float(step.get("offset_ms", first_source_offset)) - first_source_offset,
            )) * timing_scale
            target_start = replay_started + relative_ms / 1000.0
            remaining = target_start - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            actual_start = time.monotonic()
            before = client.status()
            after, metadata = execute_step(client, step, before)
            completed = time.monotonic()
            if step.get("risk", "read_only") == "read_only":
                for key in ("gateway_long_addr", "radio_profile_fingerprint_fnv1a32", "node_table_hash_fnv1a32"):
                    if baseline.get(key) and after.get(key) and baseline[key] != after[key]:
                        raise RuntimeError(
                            f"read-only replay changed {key}: {baseline[key]} -> {after[key]}"
                        )
            actual_outcome = (
                "no_response" if metadata.get("fire_and_forget")
                else classify_live_outcome(step, after)
            )
            expected_outcome = str(step.get("expected_outcome", "response"))
            if not outcome_matches(expected_outcome, actual_outcome):
                raise RuntimeError(
                    f"response divergence at step {step['index']}: "
                    f"actual={actual_outcome}, expected={expected_outcome}"
                )
            validate_live_response(step, after, metadata, actual_outcome)
            expected_subcmd = step.get("expected_response_subcmd")
            if expected_subcmd is not None and actual_outcome != "tap_local_ack_no_rf_response":
                actual = str(after.get("last_pv_ack_rsp_subcmd_hex", ""))
                if not actual or int(actual, 16) != int(expected_subcmd):
                    raise RuntimeError(
                        f"unexpected PV response at step {step['index']}: "
                        f"{actual or 'missing'}, expected 0x{int(expected_subcmd):02X}"
                    )
            results.append({
                "step": step["index"],
                "action": step["action"],
                "command_state": after.get("command_state"),
                "tap_state": after.get("tap_state"),
                "fresh_nodes": after.get("fresh_nodes"),
                "electrical_release_observed": after.get("electrical_release_observed"),
                "expected_outcome": expected_outcome,
                "actual_outcome": actual_outcome,
                "source_offset_ms": relative_ms,
                "actual_start_offset_ms": round((actual_start - replay_started) * 1000.0, 3),
                "start_late_ms": round(max(0.0, actual_start - target_start) * 1000.0, 3),
                "command_duration_ms": round((completed - actual_start) * 1000.0, 3),
                "electrical": electrical_probe.snapshot() if electrical_probe else None,
            })
            if electrical_probe:
                electrical_now = electrical_probe.snapshot()
                transitioned_now = not bool(
                    electrical_before and electrical_before["released"]
                ) and electrical_now["released"]
                if transitioned_now:
                    stopped_after_step = int(step["index"])
                    break
    finally:
        client.replay_session("stop")
    final = client.status()
    electrical_final = electrical_probe.snapshot() if electrical_probe else None
    transitioned = bool(
        electrical_before and electrical_final and
        not electrical_before["released"] and electrical_final["released"]
    )
    maintained = bool(
        electrical_before and electrical_final and
        electrical_before["released"] and electrical_final["released"]
    )
    return {
        "success": transitioned or (accept_maintained_release and maintained),
        "causal_release_transition": transitioned,
        "electrical_outcome": (
            "released_transition" if transitioned
            else "release_maintained" if maintained
            else "not_released"
        ),
        "results": results,
        "stopped_after_step": stopped_after_step,
        "initial_journal": journal,
        "electrical_initial": electrical_before,
        "electrical_final": electrical_final,
        "final_status": final,
    }


def replay_hold_offset_ms(
    plan: dict[str, Any], *, timing_scale: float, post_hold_ms: int
) -> int:
    if not plan.get("steps"):
        return 0
    last_offset = int(round(max(
        float(step.get("relative_offset_ms", 0.0)) for step in plan["steps"]
    ) * timing_scale))
    if not plan.get("selection"):
        first_request = plan.get("source_first_request_ms")
        first_release = plan.get("source_first_release_ms")
        if first_request is not None and first_release is not None:
            source_release_offset = int(round(
                max(0.0, float(first_release) - float(first_request)) * timing_scale
            ))
            return max(last_offset, source_release_offset)
    return last_offset + max(0, int(round(post_hold_ms * timing_scale)))


def start_trace_replay_checked(
    client: ReplayClient,
    *,
    attempts: int = 4,
    retry_delay_s: float = 0.2,
    **values: Any,
) -> dict[str, Any]:
    """Arm a loaded plan without blindly repeating a lost POST response."""
    before = client.trace_replay()
    if before.get("active") or before.get("state") != "loaded":
        raise RuntimeError(f"ESP replay plan is not idle and loaded: {before}")
    previous_started = int(before.get("started_ms", 0))
    expected_steps = int(before.get("step_count", 0))
    last_error: Optional[BaseException] = None
    for attempt in range(max(1, attempts)):
        try:
            response = client.trace_replay_plan("start", **values)
            if not response.get("ok", False):
                raise RuntimeError(f"ESP refused scheduled replay start: {response}")
            return response
        except (OSError, urllib.error.URLError) as exc:
            last_error = exc
            time.sleep(max(0.0, retry_delay_s))
            observed = client.trace_replay()
            observed_started = int(observed.get("started_ms", 0))
            accepted = (
                int(observed.get("step_count", 0)) == expected_steps
                and observed_started != previous_started
                and observed.get("state") != "loaded"
            )
            if accepted:
                return {
                    "ok": True,
                    "inferred_after_lost_response": True,
                    "trace_status": observed,
                }
            if observed.get("active") or observed.get("state") != "loaded":
                raise RuntimeError(
                    f"ambiguous scheduler state after lost start response: {observed}"
                ) from exc
            if attempt + 1 >= attempts:
                break
    raise RuntimeError(
        f"ESP did not accept replay start after {max(1, attempts)} checked attempts"
    ) from last_error


def abort_trace_replay_if_active(
    client: ReplayClient, *, attempts: int = 4, retry_delay_s: float = 0.1
) -> dict[str, Any]:
    """Abort without turning the normal scheduler-completion race into a failure."""
    last_error: Optional[BaseException] = None
    for attempt in range(max(1, attempts)):
        observed = client.trace_replay()
        if not observed.get("active"):
            return observed
        try:
            client.trace_replay_plan("abort")
        except urllib.error.HTTPError as exc:
            if exc.code != 409:
                raise
            last_error = exc
        except (OSError, urllib.error.URLError) as exc:
            last_error = exc
        time.sleep(max(0.0, retry_delay_s))
        observed = client.trace_replay()
        if not observed.get("active"):
            return observed
        if attempt + 1 >= attempts:
            break
    raise RuntimeError("ESP trace scheduler remained active after checked abort") from last_error


def clear_trace_replay_checked(
    client: ReplayClient, *, attempts: int = 4, retry_delay_s: float = 0.2
) -> dict[str, Any]:
    last_error: Optional[BaseException] = None
    for attempt in range(max(1, attempts)):
        try:
            response = client.trace_replay_plan("clear")
            if not response.get("ok", False):
                raise RuntimeError(f"ESP refused replay plan clear: {response}")
            return response
        except (OSError, urllib.error.URLError) as exc:
            last_error = exc
            time.sleep(max(0.0, retry_delay_s))
            observed = client.trace_replay()
            if not observed.get("active") and int(observed.get("step_count", 0)) == 0:
                return {"ok": True, "inferred_after_lost_response": True}
            if observed.get("active"):
                raise RuntimeError("cannot clear an active ESP replay") from exc
            if attempt + 1 >= attempts:
                break
    raise RuntimeError("ESP replay plan clear could not be confirmed") from last_error


def append_trace_replay_checked(
    client: ReplayClient,
    encoded: dict[str, Any],
    *,
    attempts: int = 8,
    retry_delay_s: float = 0.2,
) -> dict[str, Any]:
    requested_index = int(encoded["index"])
    last_error: Optional[BaseException] = None
    for attempt in range(max(1, attempts)):
        try:
            response = client.trace_replay_plan("append", **encoded)
            if not response.get("ok", False):
                raise RuntimeError(f"append rejected: {response}")
            return response
        except (OSError, urllib.error.URLError) as exc:
            last_error = exc
            time.sleep(max(0.0, retry_delay_s))
            observed = client.trace_replay()
            count = int(observed.get("step_count", 0))
            if count > requested_index:
                return {
                    "ok": True,
                    "index": requested_index,
                    "inferred_after_lost_response": True,
                }
            if observed.get("active") or count != requested_index:
                raise RuntimeError(
                    f"ambiguous append state index={requested_index} count={count}: {observed}"
                ) from exc
            if attempt + 1 >= attempts:
                break
    raise RuntimeError(
        f"ESP replay append index {requested_index} could not be confirmed"
    ) from last_error


def upload_plan_to_esp(
    client: ReplayClient,
    plan: dict[str, Any],
    status: dict[str, Any],
    *,
    timing_scale: float,
) -> list[dict[str, Any]]:
    clear_trace_replay_checked(client)
    encoded_steps: list[dict[str, Any]] = []
    for upload_index, step in enumerate(plan["steps"]):
        encoded = encode_firmware_replay_step(
            client, step, status, timing_scale=timing_scale, upload_index=upload_index
        )
        append_trace_replay_checked(client, encoded)
        encoded_steps.append(encoded)
        time.sleep(0.02)
    return encoded_steps


def execute_plan_on_esp(
    plan: dict[str, Any],
    *,
    base_url: str,
    timing_scale: float,
    allow_state_changing: bool,
    allow_active_rf: bool = False,
    electrical_probe: Optional[ElectricalMqttProbe] = None,
    accept_maintained_release: bool = False,
    max_late_ms: int = 250,
    poll_interval_ms: int = 15,
    post_hold_ms: int = 5000,
) -> dict[str, Any]:
    client = ReplayClient(base_url)
    initial = client.status()
    journal = client.journal()
    source_euis = set(plan.get("source_tap_euis", []))
    live_eui = str(initial.get("gateway_long_addr", "")).upper()
    if source_euis and live_eui not in source_euis:
        raise RuntimeError(f"TAP identity mismatch: live={live_eui}, source={sorted(source_euis)}")
    for step in plan["steps"]:
        if step.get("state_changing") and not allow_state_changing:
            raise PermissionError(
                f"step {step['index']} is state-changing; pass --allow-state-changing explicitly"
            )
        if step.get("risk") == "active_rf" and not (allow_active_rf or allow_state_changing):
            raise PermissionError(
                f"step {step['index']} actively addresses an optimizer; pass --allow-active-rf explicitly"
            )
        if step.get("risk", "unknown") == "unknown" and not allow_state_changing:
            raise PermissionError(
                f"step {step['index']} has unknown protocol semantics; "
                "pass --allow-state-changing only after manual review"
            )
    baseline = {
        "gateway_id_hex": initial.get("gateway_id_hex"),
        "gateway_long_addr": live_eui,
        "radio_profile_fingerprint_fnv1a32": initial.get("radio_profile_fingerprint_fnv1a32"),
        "node_table_hash_fnv1a32": initial.get("node_table_hash_fnv1a32"),
    }
    electrical_before = electrical_probe.snapshot() if electrical_probe else None
    encoded_steps = upload_plan_to_esp(
        client, plan, initial, timing_scale=timing_scale
    )
    hold_until_ms = replay_hold_offset_ms(
        plan, timing_scale=timing_scale, post_hold_ms=post_hold_ms
    )
    start_delay_ms = 250
    transitioned_during_run = False
    stopped_after_step: Optional[int] = None
    trace_status: dict[str, Any] = {}
    try:
        start_trace_replay_checked(
            client,
            allow_state_changing=str(bool(allow_state_changing)).lower(),
            allow_active_rf=str(bool(allow_active_rf)).lower(),
            max_late_ms=max_late_ms,
            poll_interval_ms=poll_interval_ms,
            poll_guard_ms=min(8, max(2, poll_interval_ms // 2)),
            start_delay_ms=start_delay_ms,
            hold_until_ms=hold_until_ms,
        )
        expected_end = time.monotonic() + (start_delay_ms + hold_until_ms) / 1000.0
        while time.monotonic() < expected_end + 0.5:
            if electrical_probe:
                current = electrical_probe.snapshot()
                if bool(electrical_before and not electrical_before["released"] and current["released"]):
                    transitioned_during_run = True
                    trace_status = client.trace_replay()
                    stopped_after_step = int(trace_status.get("step_index", 0))
                    if trace_status.get("active"):
                        trace_status = abort_trace_replay_if_active(client)
                    break
            time.sleep(0.1)
        if not trace_status:
            trace_status = client.trace_replay()
        deadline = time.monotonic() + 12.0
        while trace_status.get("active") and time.monotonic() < deadline:
            time.sleep(0.3)
            trace_status = client.trace_replay()
        if trace_status.get("active"):
            trace_status = abort_trace_replay_if_active(client)
            raise TimeoutError("ESP trace scheduler did not finish its hold window")
    except BaseException:
        try:
            active = client.trace_replay()
            if active.get("active"):
                abort_trace_replay_if_active(client)
        except BaseException:
            pass
        raise

    results = []
    for index in range(len(encoded_steps)):
        try:
            result = client.trace_replay_result(index)
        except Exception:
            break
        if result.get("valid"):
            result["source_step"] = plan["steps"][index]["index"]
            results.append(result)
    final = client.status()
    electrical_final = electrical_probe.snapshot() if electrical_probe else None
    transitioned = transitioned_during_run or bool(
        electrical_before and electrical_final and
        not electrical_before["released"] and electrical_final["released"]
    )
    maintained = bool(
        electrical_before and electrical_final and
        electrical_before["released"] and electrical_final["released"]
    )
    scheduler_ok = str(trace_status.get("state")) == "complete" or transitioned
    criterion_ok = transitioned or (accept_maintained_release and maintained)
    return {
        "success": scheduler_ok and criterion_ok,
        "transport": "esp_scheduled",
        "scheduler_ok": scheduler_ok,
        "causal_release_transition": transitioned,
        "electrical_outcome": (
            "released_transition" if transitioned
            else "release_maintained" if maintained
            else "not_released"
        ),
        "results": results,
        "stopped_after_step": stopped_after_step,
        "encoded_steps": encoded_steps,
        "initial_journal": journal,
        "baseline": baseline,
        "electrical_initial": electrical_before,
        "electrical_final": electrical_final,
        "trace_status": trace_status,
        "final_status": final,
    }


def ddmin(items: list[Any], test: Callable[[list[Any]], bool]) -> list[Any]:
    """Classic delta debugging; caller must restore identical hardware state per test."""
    candidate = list(items)
    granularity = 2
    while len(candidate) >= 2:
        chunk_size = (len(candidate) + granularity - 1) // granularity
        reduced = False
        for start in range(0, len(candidate), chunk_size):
            complement = candidate[:start] + candidate[start + chunk_size :]
            if complement and test(complement):
                candidate = complement
                granularity = max(granularity - 1, 2)
                reduced = True
                break
        if reduced:
            continue
        if granularity >= len(candidate):
            break
        granularity = min(len(candidate), granularity * 2)
    return candidate


def select_plan(
    plan: dict[str, Any], *, blocks: Optional[set[int]] = None, steps: Optional[set[int]] = None
) -> dict[str, Any]:
    selected = [
        step for step in plan["steps"]
        if (blocks is None or int(step["block"]) in blocks)
        and (steps is None or int(step["index"]) in steps)
    ]
    if not selected:
        raise ValueError("plan selection is empty")
    result = copy.deepcopy(plan)
    first_offset = float(selected[0].get("relative_offset_ms", selected[0].get("offset_ms", 0.0)))
    result["steps"] = copy.deepcopy(selected)
    for step in result["steps"]:
        source_offset = float(step.get("relative_offset_ms", step.get("offset_ms", 0.0)))
        step["relative_offset_ms"] = round(source_offset - first_offset, 3)
    result["selection"] = {
        "blocks": sorted(blocks) if blocks is not None else None,
        "steps": sorted(steps) if steps is not None else None,
    }
    return result


def parse_int_set(text: Optional[str]) -> Optional[set[int]]:
    if text is None:
        return None
    values = {int(item.strip(), 0) for item in text.split(",") if item.strip()}
    if not values:
        raise ValueError("selection must contain at least one integer")
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, help="build a plan from this successful trace")
    parser.add_argument("--plan", type=Path, help="load an existing replay plan")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--result-output", type=Path)
    parser.add_argument("--all", action="store_true", help="do not stop plan extraction at first released Vout")
    parser.add_argument("--blocks", help="comma-separated replay block IDs")
    parser.add_argument("--steps", help="comma-separated source step IDs")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument(
        "--transport",
        choices=("esp", "http"),
        default="esp",
        help="execute timing-sensitive plans on the ESP scheduler (default) or via legacy HTTP steps",
    )
    parser.add_argument("--base-url", default="http://opentaptox-esp32c6.local")
    parser.add_argument("--timing-scale", type=float, default=1.0)
    parser.add_argument("--max-late-ms", type=int, default=250)
    parser.add_argument("--replay-poll-ms", type=int, default=15)
    parser.add_argument("--post-hold-ms", type=int, default=5000)
    parser.add_argument("--allow-active-rf", action="store_true")
    parser.add_argument("--allow-state-changing", action="store_true")
    parser.add_argument("--mqtt-broker")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument(
        "--electrical-power-topic",
        default="modbus-to-mqtt/devices/Growatt_MIC/iregs/input_power/value",
    )
    parser.add_argument(
        "--electrical-voltage-topic",
        default="modbus-to-mqtt/devices/Growatt_MIC/iregs/pv1_voltage/value",
    )
    parser.add_argument("--min-electrical-power-w", type=float, default=100.0)
    parser.add_argument("--min-electrical-voltage-v", type=float, default=100.0)
    parser.add_argument("--accept-maintained-release", action="store_true")
    args = parser.parse_args()
    if bool(args.trace) == bool(args.plan):
        parser.error("provide exactly one of --trace or --plan")
    plan = build_replay_plan(args.trace, through_first_release=not args.all) if args.trace else json.loads(
        args.plan.read_text(encoding="utf-8")
    )
    if args.blocks or args.steps:
        plan = select_plan(
            plan,
            blocks=parse_int_set(args.blocks),
            steps=parse_int_set(args.steps),
        )
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    if args.execute:
        if not args.mqtt_broker:
            parser.error("--execute requires --mqtt-broker for independent electrical verification")
        probe = ElectricalMqttProbe(
            args.mqtt_broker,
            args.mqtt_port,
            args.electrical_power_topic,
            args.electrical_voltage_topic,
            args.min_electrical_power_w,
            args.min_electrical_voltage_v,
        )
        try:
            if args.transport == "esp":
                result = execute_plan_on_esp(
                    plan,
                    base_url=args.base_url,
                    timing_scale=args.timing_scale,
                    allow_state_changing=args.allow_state_changing,
                    allow_active_rf=args.allow_active_rf,
                    electrical_probe=probe,
                    accept_maintained_release=args.accept_maintained_release,
                    max_late_ms=args.max_late_ms,
                    poll_interval_ms=args.replay_poll_ms,
                    post_hold_ms=args.post_hold_ms,
                )
            else:
                result = execute_plan(
                    plan,
                    base_url=args.base_url,
                    timing_scale=args.timing_scale,
                    allow_state_changing=args.allow_state_changing,
                    allow_active_rf=args.allow_active_rf,
                    electrical_probe=probe,
                    accept_maintained_release=args.accept_maintained_release,
                )
        finally:
            probe.close()
        if args.result_output:
            args.result_output.parent.mkdir(parents=True, exist_ok=True)
            args.result_output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))
        return 0 if result.get("success") else 2
    if not args.output:
        print(json.dumps(plan, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
