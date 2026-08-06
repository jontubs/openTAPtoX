#!/usr/bin/env python3
"""Capture TAP status, event-ring entries, and interesting frames via HTTP."""

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


def get_json(base_url: str, path: str) -> dict:
    with urllib.request.urlopen(base_url.rstrip("/") + path, timeout=5) as response:
        return json.load(response)


def append_jsonl(path: Path, kind: str, data: object, started: float) -> None:
    record = {
        "ts": timestamp(),
        "elapsed_s": round(time.monotonic() - started, 3),
        "kind": kind,
        "data": data,
    }
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://opentaptox-esp32c6.local")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--status-interval", type=float, default=5.0)
    parser.add_argument("--duration", type=float, default=0.0)
    parser.add_argument("--output-prefix", required=True)
    args = parser.parse_args()

    prefix = Path(args.output_prefix)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    status_path = Path(str(prefix) + ".status.jsonl")
    events_path = Path(str(prefix) + ".events.jsonl")
    frames_path = Path(str(prefix) + ".interesting.jsonl")
    started = time.monotonic()
    last_status = 0.0
    since_seq = 0
    seen_events: set[tuple[int, str]] = set()

    append_jsonl(
        status_path,
        "capture_start",
        {
            "base_url": args.base_url,
            "interval_s": args.interval,
            "status_interval_s": args.status_interval,
            "duration_s": args.duration,
        },
        started,
    )

    try:
        while args.duration <= 0 or time.monotonic() - started < args.duration:
            elapsed = time.monotonic() - started
            if elapsed - last_status >= args.status_interval:
                try:
                    append_jsonl(status_path, "status", get_json(args.base_url, "/api/status"), started)
                except Exception as exc:
                    append_jsonl(status_path, "status_error", {"error": str(exc)}, started)
                last_status = elapsed

            try:
                query = urllib.parse.urlencode({"since_seq": since_seq})
                batch = get_json(args.base_url, "/api/interesting-frames?" + query)
                head_seq = int(batch.get("head_seq", since_seq))
                if head_seq < since_seq:
                    append_jsonl(
                        status_path,
                        "sequence_reset",
                        {"previous_seq": since_seq, "new_head_seq": head_seq},
                        started,
                    )
                    since_seq = 0
                    batch = get_json(args.base_url, "/api/interesting-frames?since_seq=0")
                for frame in batch.get("frames", []):
                    append_jsonl(frames_path, "interesting_frame", frame, started)
                    since_seq = max(since_seq, int(frame.get("seq", since_seq)))
            except Exception as exc:
                append_jsonl(status_path, "interesting_frames_error", {"error": str(exc)}, started)

            try:
                for event in get_json(args.base_url, "/api/events").get("events", []):
                    key = (int(event.get("ms", 0)), str(event.get("text", "")))
                    if key not in seen_events:
                        seen_events.add(key)
                        append_jsonl(events_path, "event", event, started)
            except Exception as exc:
                append_jsonl(status_path, "events_error", {"error": str(exc)}, started)

            time.sleep(max(args.interval, 0.1))
    except KeyboardInterrupt:
        pass
    finally:
        append_jsonl(status_path, "capture_stop", {}, started)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
