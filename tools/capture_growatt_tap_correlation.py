#!/usr/bin/env python3
"""Capture Growatt MQTT values and openTAPtoX status on one timestamped timeline."""

from __future__ import annotations

import argparse
import json
import threading
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt


GROWATT_BASE = "modbus-to-mqtt/devices/Growatt_MIC/iregs/"
MQTT_TOPICS = [
    GROWATT_BASE + name + "/value"
    for name in (
        "inverter_status",
        "input_power",
        "pv1_voltage",
        "pv1_current",
        "pv1_input_power",
        "output_power",
    )
] + [
    "modbus-to-mqtt/debug/read_result",
    "modbus-to-mqtt/debug/error_count",
    "modbus-to-mqtt/debug/last_error",
]

STATUS_FIELDS = (
    "firmware_version",
    "uptime_ms",
    "tap_link_up",
    "tap_link_age_ms",
    "polls_sent",
    "poll_timeouts",
    "node_count",
    "node_confirmed_count",
    "node_pending_count",
    "power_count",
    "network_mode",
    "network_countdown",
    "network_flags",
    "network_confirmed_nodes",
    "network_expected_nodes",
    "network_configured_nodes",
    "network_active_nodes",
    "last_pv_subcmd_hex",
    "last_pv_request_hex",
    "last_pv_ack_hex",
    "command_busy",
    "command_name",
    "command_state",
)


def timestamp() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://opentaptox-esp32c6.local")
    parser.add_argument("--broker", default="localhost")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--duration", type=float, default=0.0)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    started = time.monotonic()

    def write(kind: str, data: object) -> None:
        row = {
            "ts": timestamp(),
            "elapsed_s": round(time.monotonic() - started, 3),
            "kind": kind,
            "data": data,
        }
        with lock:
            with output.open("a", encoding="utf-8", newline="\n") as stream:
                stream.write(json.dumps(row, separators=(",", ":"), sort_keys=True) + "\n")

    def on_connect(client, userdata, flags, result_code):
        write("mqtt_connect", {"result_code": result_code})
        for topic in MQTT_TOPICS:
            client.subscribe(topic)

    def on_message(client, userdata, message):
        write(
            "mqtt",
            {
                "topic": message.topic,
                "value": message.payload.decode("utf-8", "replace"),
                "retained": bool(message.retain),
            },
        )

    write(
        "capture_start",
        {
            "base_url": args.base_url,
            "broker": args.broker,
            "port": args.port,
            "interval_s": args.interval,
            "duration_s": args.duration,
        },
    )

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 10)
    client.loop_start()

    try:
        next_status = 0.0
        while args.duration <= 0 or time.monotonic() - started < args.duration:
            elapsed = time.monotonic() - started
            if elapsed >= next_status:
                try:
                    url = args.base_url.rstrip("/") + "/api/status"
                    with urllib.request.urlopen(url, timeout=5) as response:
                        status = json.load(response)
                    write("tap_status", {key: status.get(key) for key in STATUS_FIELDS})
                except Exception as exc:  # Keep the capture alive across reboots.
                    write("tap_status_error", {"error": str(exc)})
                next_status += max(args.interval, 0.1)
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()
        write("capture_stop", {})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
