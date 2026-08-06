#!/usr/bin/env python3
"""Capture selected MQTT topic trees as timestamped JSONL."""

from __future__ import annotations

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt


def timestamp() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--broker", default="localhost")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--topic", action="append", required=True)
    parser.add_argument("--duration", type=float, default=0.0)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()

    def write(record: dict) -> None:
        record["ts"] = timestamp()
        with output.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n")

    def on_connect(client, userdata, flags, result_code):
        write({"kind": "mqtt_connect", "data": {"result_code": result_code}})
        for topic in args.topic:
            client.subscribe(topic)

    def on_message(client, userdata, message):
        value = message.payload.decode("utf-8", "replace")
        if message.topic.endswith("/raw/frame"):
            try:
                frame = json.loads(value)
            except json.JSONDecodeError:
                frame = None
            if isinstance(frame, dict):
                write({"frame": frame, "topic": message.topic})
                return
        write(
            {
                "kind": "mqtt",
                "data": {
                    "topic": message.topic,
                    "value": value,
                    "retained": bool(message.retain),
                },
            }
        )

    write(
        {
            "kind": "capture_start",
            "data": {
                "broker": args.broker,
                "port": args.port,
                "topics": args.topic,
                "duration_s": args.duration,
            },
        }
    )
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 10)
    client.loop_start()

    try:
        while args.duration <= 0 or time.monotonic() - started < args.duration:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()
        write({"kind": "capture_stop", "data": {}})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
