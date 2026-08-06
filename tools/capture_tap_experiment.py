#!/usr/bin/env python3
"""Capture TAP MQTT, HTTP diagnostics, and Growatt values in one process."""

from __future__ import annotations

import argparse
import json
import threading
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt


GROWATT_TOPIC = "modbus-to-mqtt/devices/Growatt_MIC/iregs/+/value"


def timestamp() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://opentaptox-esp32c6.local")
    parser.add_argument("--broker", default="localhost")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--duration", type=float, default=0.0)
    parser.add_argument("--http-interval", type=float, default=2.0)
    parser.add_argument("--status-interval", type=float, default=10.0)
    parser.add_argument("--output-prefix", required=True)
    args = parser.parse_args()

    prefix = Path(args.output_prefix)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    paths = {
        "raw": Path(str(prefix) + ".raw_mqtt.jsonl"),
        "correlation": Path(str(prefix) + ".correlation.jsonl"),
        "status": Path(str(prefix) + ".status.jsonl"),
        "events": Path(str(prefix) + ".events.jsonl"),
        "interesting": Path(str(prefix) + ".interesting.jsonl"),
    }
    started = time.monotonic()
    lock = threading.Lock()

    def write(target: str, kind: str, data: object) -> None:
        row = {
            "ts": timestamp(),
            "elapsed_s": round(time.monotonic() - started, 3),
            "kind": kind,
            "data": data,
        }
        with lock:
            with paths[target].open("a", encoding="utf-8", newline="\n") as stream:
                stream.write(json.dumps(row, separators=(",", ":"), sort_keys=True) + "\n")

    def get_json(path: str) -> dict:
        url = args.base_url.rstrip("/") + path
        with urllib.request.urlopen(url, timeout=5) as response:
            return json.load(response)

    def on_connect(client, userdata, flags, result_code):
        write("status", "mqtt_connect", {"result_code": result_code})
        client.subscribe("openTAPtoX/esp32c6/raw/frame")
        client.subscribe("openTAPtoX/esp32c6/status/#")
        client.subscribe(GROWATT_TOPIC)

    def on_message(client, userdata, message):
        value = message.payload.decode("utf-8", "replace")
        if message.topic == "openTAPtoX/esp32c6/raw/frame":
            try:
                value = json.loads(value)
            except json.JSONDecodeError:
                pass
            write("raw", "mqtt", {"topic": message.topic, "value": value})
        elif message.topic.startswith("modbus-to-mqtt/"):
            write("correlation", "mqtt", {"topic": message.topic, "value": value})
        else:
            write("status", "mqtt", {"topic": message.topic, "value": value})

    metadata = {
        "base_url": args.base_url,
        "broker": args.broker,
        "duration_s": args.duration,
        "http_interval_s": args.http_interval,
        "status_interval_s": args.status_interval,
    }
    for target in paths:
        write(target, "capture_start", metadata)

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 10)
    client.loop_start()

    last_status = -args.status_interval
    since_seq = 0
    seen_events: set[tuple[int, str]] = set()
    try:
        while args.duration <= 0 or time.monotonic() - started < args.duration:
            elapsed = time.monotonic() - started
            if elapsed - last_status >= args.status_interval:
                try:
                    status = get_json("/api/status")
                    write("status", "status", status)
                    write("correlation", "tap_status", status)
                except Exception as exc:
                    write("status", "status_error", {"error": str(exc)})
                last_status = elapsed

            try:
                query = urllib.parse.urlencode({"since_seq": since_seq})
                batch = get_json("/api/interesting-frames?" + query)
                head_seq = int(batch.get("head_seq", since_seq))
                if head_seq < since_seq:
                    write(
                        "status",
                        "sequence_reset",
                        {"previous_seq": since_seq, "new_head_seq": head_seq},
                    )
                    since_seq = 0
                    batch = get_json("/api/interesting-frames?since_seq=0")
                for frame in batch.get("frames", []):
                    write("interesting", "interesting_frame", frame)
                    since_seq = max(since_seq, int(frame.get("seq", since_seq)))
            except Exception as exc:
                write("status", "interesting_frames_error", {"error": str(exc)})

            try:
                for event in get_json("/api/events").get("events", []):
                    key = (int(event.get("ms", 0)), str(event.get("text", "")))
                    if key not in seen_events:
                        seen_events.add(key)
                        write("events", "event", event)
            except Exception as exc:
                write("status", "events_error", {"error": str(exc)})

            time.sleep(max(args.http_interval, 0.2))
    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        client.disconnect()
        for target in paths:
            write(target, "capture_stop", {})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
