#!/usr/bin/env python3
"""Try each configured optimizer alone in a one-node TAP learn window."""

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
    parser.add_argument("--seconds-per-optimizer", type=float, default=120.0)
    parser.add_argument("--start-index", type=int, default=1)
    parser.add_argument(
        "--join-seed",
        required=True,
        help="profile-specific 24-byte hex value; keep it local and never commit it",
    )
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    join_seed = args.join_seed.strip().upper()
    if len(join_seed) != 48 or any(char not in "0123456789ABCDEF" for char in join_seed):
        parser.error("--join-seed must contain exactly 48 hexadecimal characters")

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
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
        for _ in range(30):
            req = urllib.request.Request(
                args.base_url.rstrip("/") + path,
                data=b"" if method == "POST" else None,
                method=method,
            )
            try:
                with urllib.request.urlopen(req, timeout=5) as response:
                    return json.load(response)
            except Exception as exc:
                last_error = exc
                time.sleep(0.2)
        raise RuntimeError(f"endpoint unavailable: {path}: {last_error}")

    def status() -> dict:
        last_error: Exception | None = None
        for _ in range(30):
            try:
                return request("/api/status")
            except Exception as exc:
                last_error = exc
                time.sleep(0.2)
        raise RuntimeError(f"status endpoint unavailable: {last_error}")

    def wait_idle(timeout: float = 12.0) -> dict:
        deadline = time.monotonic() + timeout
        last = {}
        while time.monotonic() < deadline:
            last = status()
            if not last.get("command_busy"):
                return last
            time.sleep(0.2)
        raise TimeoutError(f"TAP command remained busy: {last.get('command_state')}")

    def command(path: str, method: str = "GET") -> dict:
        deadline = time.monotonic() + 15.0
        last_error = ""
        while time.monotonic() < deadline:
            wait_idle()
            try:
                result = request(path, method)
            except Exception as exc:
                last_error = str(exc)
                time.sleep(0.25)
                continue
            if result.get("ok"):
                write("command", {"path": path, "response": result})
                wait_idle()
                return result
            last_error = str(result)
            time.sleep(0.25)
        raise RuntimeError(f"command failed: {path}: {last_error}")

    panel_map = request("/api/panel-map").get("panel_map", [])
    panels = [
        {"label": item.get("label", ""), "long_addr": item.get("long_addr", "")}
        for item in panel_map
        if len(item.get("long_addr", "")) == 16
    ]
    write(
        "sweep_start",
        {
            "seconds_per_optimizer": args.seconds_per_optimizer,
            "start_index": args.start_index,
            "panels": panels,
        },
    )

    try:
        for index, panel in enumerate(panels, start=1):
            if index < args.start_index:
                continue
            label = panel["label"]
            long_addr = panel["long_addr"]
            write("optimizer_start", {"index": index, "label": label, "long_addr": long_addr})

            command("/api/command/hold-learn?confirm=HOLD_LEARN", "POST")
            command("/api/command/pv-subcmd?subcmd=0x2B&body_hex=BABE")
            command("/api/command/pv-subcmd?subcmd=0x22&body_hex=0001")
            seed_body = "01" + long_addr + "8002"
            command("/api/command/pv-subcmd?" + urllib.parse.urlencode({"subcmd": "0x29", "body_hex": seed_body}))
            command("/api/command/node-table?start=0")
            node_map = request("/api/node-map")
            write("node_table_readback", node_map)
            command("/api/command/pv-subcmd?subcmd=0x2D&body_hex=BABE02038400010100")
            command("/api/command/pv-subcmd?" + urllib.parse.urlencode({"subcmd": "0x41", "body_hex": join_seed}))

            optimizer_started = time.monotonic()
            next_status = optimizer_started
            next_seed = optimizer_started + 60.0
            info_sent = False
            detected = False
            while time.monotonic() - optimizer_started < args.seconds_per_optimizer:
                now = time.monotonic()
                if now >= next_status:
                    command("/api/command/network-status")
                    current = status()
                    nodes = request("/api/node-map").get("nodes", [])
                    write("observation", {"label": label, "status": current, "nodes": nodes})
                    if (current.get("network_confirmed_nodes", 0) > 0 or
                            current.get("power_count", 0) > 0 or
                            any(node.get("rf_confirmed") or node.get("has_power") for node in nodes)):
                        detected = True
                        break
                    next_status = now + 10.0
                if now >= next_seed:
                    command("/api/command/pv-subcmd?" + urllib.parse.urlencode({"subcmd": "0x41", "body_hex": join_seed}))
                    next_seed = now + 60.0
                if not info_sent and now - optimizer_started >= 45.0:
                    command("/api/command/node-text?" + urllib.parse.urlencode({
                        "nodeId": "2", "text": "^00Info", "appendCr": "true"
                    }))
                    info_sent = True
                time.sleep(0.2)

            write(
                "optimizer_result",
                {
                    "index": index,
                    "label": label,
                    "long_addr": long_addr,
                    "detected": detected,
                    "status": status(),
                    "nodes": request("/api/node-map").get("nodes", []),
                },
            )
    finally:
        try:
            command("/api/command/hold-learn?confirm=HOLD_LEARN", "POST")
            command("/api/command/rebuild-node-table", "POST")
            write("restore_full_table", {"ok": True, "status": status()})
        except Exception as exc:
            write("restore_full_table", {"ok": False, "error": str(exc)})
        write("sweep_stop", {})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
