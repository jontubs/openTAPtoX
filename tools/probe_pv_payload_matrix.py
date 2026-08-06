#!/usr/bin/env python3
"""Probe selected TAP PV subcommands with explicit payloads.

This is an active reverse-engineering tool. It suspends the automatic wake
sequence before each short batch so delayed maintenance replies cannot be
mistaken for replies to the command under test.
"""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_BODIES = (
    "",
    "00",
    "01",
    "FF",
    "0000",
    "0001",
    "0002",
    "000A",
    "8002",
    "BABE",
    "37249266",
)


def timestamp() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://opentaptox-esp32c6.local")
    parser.add_argument(
        "--subcmd",
        action="append",
        required=True,
        type=lambda value: int(value, 0),
        help="PV subcommand; repeat for multiple values",
    )
    parser.add_argument(
        "--body",
        action="append",
        dest="bodies",
        help="hex payload; repeat for multiple values (defaults to a curated set)",
    )
    parser.add_argument("--settle-seconds", type=float, default=0.35)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    if any(not 0 <= subcmd <= 0xFF for subcmd in args.subcmd):
        parser.error("subcommands must fit in one byte")
    bodies = args.bodies if args.bodies is not None else list(DEFAULT_BODIES)
    for body in bodies:
        if len(body) % 2 or any(char not in "0123456789abcdefABCDEF" for char in body):
            parser.error(f"invalid hex body: {body!r}")

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    base_url = args.base_url.rstrip("/")
    started = time.monotonic()

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
        last_error: Exception | None = None
        for _ in range(20):
            req = urllib.request.Request(base_url + path, method=method)
            try:
                with urllib.request.urlopen(req, timeout=5) as response:
                    return json.load(response)
            except urllib.error.HTTPError as exc:
                try:
                    data = json.load(exc)
                except Exception:
                    data = {"ok": False, "error": f"HTTP {exc.code}"}
                data["http_status"] = exc.code
                return data
            except (OSError, TimeoutError) as exc:
                last_error = exc
                time.sleep(0.25)
        raise RuntimeError(f"endpoint unavailable: {path}: {last_error}")

    def status() -> dict:
        return request("/api/status")

    def wait_idle(timeout: float = 8.0) -> dict:
        deadline = time.monotonic() + timeout
        current: dict = {}
        while time.monotonic() < deadline:
            current = status()
            if not current.get("command_busy"):
                return current
            time.sleep(0.1)
        return current

    def snapshot(current: dict) -> dict:
        keys = (
            "command_busy",
            "command_name",
            "command_state",
            "tap_link_up",
            "frames_rx",
            "frames_crc_error",
            "tap_responses_rx",
            "polls_sent",
            "poll_timeouts",
            "node_wake_active",
            "node_wake_completed",
            "node_count",
            "node_confirmed_count",
            "network_mode",
            "network_countdown",
            "network_active_nodes",
            "power_count",
            "next_packet_hex",
            "last_frame_type_code_hex",
            "last_frame_payload_preview_hex",
            "last_pv_subcmd_hex",
            "last_pv_request_hex",
            "last_pv_ack_hex",
            "last_pv_ack_status_hex",
            "last_pv_ack_rsp_subcmd_hex",
            "last_pv_ack_body_hex",
            "radio_channel",
            "radio_pan_id_hex",
            "radio_profile_fingerprint_fnv1a32",
        )
        return {key: current.get(key) for key in keys}

    # Finish any automatic recovery and push its next retry ten minutes away.
    idle = wait_idle(30.0)
    hold = request("/api/command/hold-learn?confirm=HOLD_LEARN", method="POST")
    if not hold.get("ok"):
        idle = wait_idle(30.0)
        hold = request("/api/command/hold-learn?confirm=HOLD_LEARN", method="POST")
    wait_idle(10.0)
    write("matrix_start", {"hold": hold, "status": snapshot(status())})

    for subcmd in args.subcmd:
        for body in bodies:
            before = wait_idle()
            query = {"subcmd": f"0x{subcmd:02X}"}
            if body:
                query["body_hex"] = body.upper()
            queued = request("/api/command/pv-subcmd?" + urllib.parse.urlencode(query))
            after = wait_idle()
            time.sleep(max(args.settle_seconds, 0.0))
            after = status()
            row = {
                "subcmd": subcmd,
                "subcmd_hex": f"0x{subcmd:02X}",
                "body_hex": body.upper(),
                "queued": queued,
                "before": snapshot(before),
                "after": snapshot(after),
            }
            write("probe", row)
            print(
                f"{subcmd:02X}/{body.upper() or '<empty>'} "
                f"queued={queued.get('ok')} state={after.get('command_state')} "
                f"ack={after.get('last_pv_ack_hex')} "
                f"rsp={after.get('last_pv_ack_rsp_subcmd_hex')} "
                f"body={after.get('last_pv_ack_body_hex')} "
                f"frames+={after.get('frames_rx', 0) - before.get('frames_rx', 0)} "
                f"power={after.get('power_count')}",
                flush=True,
            )

    write("matrix_stop", {"status": snapshot(status())})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
