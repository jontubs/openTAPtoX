#!/usr/bin/env python3
"""Capture a serial text stream with host timestamps."""

from __future__ import annotations

import argparse
import signal
import sys
from datetime import datetime
from pathlib import Path

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    stopping = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    with serial.Serial(args.port, args.baud, timeout=0.25) as stream:
        with args.output.open("a", encoding="utf-8", buffering=1) as capture:
            while not stopping:
                raw = stream.readline()
                if not raw:
                    continue
                timestamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                record = f"{timestamp} {line}"
                print(record, flush=True)
                capture.write(record + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
