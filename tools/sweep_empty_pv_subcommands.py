#!/usr/bin/env python3
"""Probe TAP active subcommands with an empty body and record state changes."""

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
    parser.add_argument("--start", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--end", type=lambda value: int(value, 0), default=0xFF)
    parser.add_argument("--settle-seconds", type=float, default=0.15)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    if not 0 <= args.start <= args.end <= 0xFF:
        parser.error("start and end must satisfy 0 <= start <= end <= 255")

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

    def request(path: str) -> dict:
        last_error: Exception | None = None
        for _ in range(40):
            try:
                with urllib.request.urlopen(base_url + path, timeout=5) as response:
                    return json.load(response)
            except Exception as exc:
                last_error = exc
                time.sleep(0.25)
        raise RuntimeError(f"endpoint unavailable: {path}: {last_error}")

    def status() -> dict:
        return request("/api/status")

    def wait_idle(timeout: float = 8.0) -> dict:
        deadline = time.monotonic() + timeout
        last: dict = {}
        while time.monotonic() < deadline:
            last = status()
            if not last.get("command_busy"):
                return last
            time.sleep(0.1)
        return last

    def state_snapshot(current: dict) -> dict:
        keys = (
            "command_busy",
            "command_name",
            "command_state",
            "tap_link_up",
            "frames_crc_error",
            "node_count",
            "node_confirmed_count",
            "network_mode",
            "network_countdown",
            "network_active_nodes",
            "power_count",
            "next_packet_hex",
            "last_pv_ack_hex",
            "last_pv_ack_status_hex",
            "last_pv_ack_rsp_subcmd_hex",
            "last_pv_ack_body_hex",
            "radio_channel",
            "radio_pan_id_hex",
            "radio_profile_fingerprint_fnv1a32",
        )
        return {key: current.get(key) for key in keys}

    initial_status = status()
    write(
        "sweep_start",
        {
            "start": args.start,
            "end": args.end,
            "settle_seconds": args.settle_seconds,
            "status": state_snapshot(initial_status),
            "node_map": request("/api/node-map").get("nodes", []),
        },
    )

    for subcmd in range(args.start, args.end + 1):
        before = wait_idle()
        path = "/api/command/pv-subcmd?" + urllib.parse.urlencode(
            {"subcmd": f"0x{subcmd:02X}"}
        )
        try:
            queued = request(path)
            after = wait_idle()
            time.sleep(max(args.settle_seconds, 0.0))
            after = status()
            row = {
                "subcmd": subcmd,
                "subcmd_hex": f"0x{subcmd:02X}",
                "queued": queued,
                "before": state_snapshot(before),
                "after": state_snapshot(after),
            }
            write("probe", row)
            print(
                f"{subcmd:02X} state={after.get('command_state')} "
                f"ack={after.get('last_pv_ack_hex')} "
                f"rsp={after.get('last_pv_ack_rsp_subcmd_hex')} "
                f"nodes={after.get('node_confirmed_count')}/{after.get('node_count')} "
                f"power={after.get('power_count')} cursor={after.get('next_packet_hex')}",
                flush=True,
            )
        except Exception as exc:
            write(
                "probe_error",
                {
                    "subcmd": subcmd,
                    "subcmd_hex": f"0x{subcmd:02X}",
                    "error": str(exc),
                },
            )
            print(f"{subcmd:02X} error={exc}", flush=True)

    final_status = status()
    write(
        "sweep_stop",
        {
            "status": state_snapshot(final_status),
            "node_map": request("/api/node-map").get("nodes", []),
        },
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
