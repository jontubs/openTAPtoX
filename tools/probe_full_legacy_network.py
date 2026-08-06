#!/usr/bin/env python3
"""Temporarily replay a legacy all-confirmed TAP table and CCA sync cycle."""

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
    parser.add_argument("--duration", type=float, default=600.0)
    parser.add_argument("--send-join-seed", action="store_true")
    parser.add_argument(
        "--join-seed",
        required=True,
        help="profile-specific 24-byte hex value; keep it local and never commit it",
    )
    parser.add_argument("--soft-reset", action="store_true")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    join_seed = args.join_seed.strip().upper()
    if len(join_seed) != 48 or any(char not in "0123456789ABCDEF" for char in join_seed):
        parser.error("--join-seed must contain exactly 48 hexadecimal characters")

    base_url = args.base_url.rstrip("/")
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
        req = urllib.request.Request(
            base_url + path,
            data=b"" if method == "POST" else None,
            method=method,
        )
        with urllib.request.urlopen(req, timeout=8) as response:
            return json.load(response)

    def status() -> dict:
        return request("/api/status")

    def wait_idle(timeout: float = 20.0) -> dict:
        deadline = time.monotonic() + timeout
        last: dict = {}
        while time.monotonic() < deadline:
            try:
                last = status()
                if not last.get("command_busy"):
                    return last
            except Exception:
                pass
            time.sleep(0.2)
        raise TimeoutError(f"TAP command remained busy: {last.get('command_state')}")

    def command(path: str, method: str = "GET", timeout: float = 30.0) -> dict:
        deadline = time.monotonic() + timeout
        last_error = ""
        while time.monotonic() < deadline:
            wait_idle()
            try:
                result = request(path, method)
                if result.get("ok"):
                    completed = wait_idle()
                    write(
                        "command",
                        {
                            "path": path,
                            "response": result,
                            "command_state": completed.get("command_state"),
                            "ack_status": completed.get("last_pv_ack_status_hex"),
                        },
                    )
                    return completed
                last_error = str(result)
            except Exception as exc:
                last_error = str(exc)
            time.sleep(0.3)
        raise RuntimeError(f"command failed: {path}: {last_error}")

    def pv_path(subcmd: int, body_hex: str) -> str:
        return "/api/command/pv-subcmd?" + urllib.parse.urlencode(
            {"subcmd": f"0x{subcmd:02X}", "body_hex": body_hex}
        )

    panel_rows = request("/api/panel-map").get("panel_map", [])
    panels = []
    for index, item in enumerate(panel_rows):
        long_addr = str(item.get("long_addr", "")).upper()
        if len(long_addr) == 16:
            panels.append(
                {
                    "node_id": index + 2,
                    "label": str(item.get("label", "")) or f"A{index + 1}",
                    "long_addr": long_addr,
                }
            )
    if not panels:
        raise RuntimeError("panel map is empty")

    initial_nodes = request("/api/node-map").get("nodes", [])
    anchors = {
        int(node["node_id"])
        for node in initial_nodes
        if not node.get("pending", True)
        and (node.get("rf_confirmed") or node.get("has_power"))
    }
    if not anchors:
        raise RuntimeError("at least one RF-proven anchor is required")

    def table_body(low_ids: set[int]) -> str:
        body = f"{len(panels):02X}"
        for panel in panels:
            node_id = panel["node_id"]
            raw_id = node_id if node_id in low_ids else 0x8000 | node_id
            body += panel["long_addr"] + f"{raw_id:04X}"
        return body

    def restore() -> None:
        command("/api/command/hold-learn?confirm=HOLD_LEARN", "POST")
        command(pv_path(0x2B, "BABE"))
        command(pv_path(0x2D, "BABE02038400000100"))
        command(pv_path(0x22, "0001"))
        command(pv_path(0x29, table_body(anchors)))
        command("/api/command/node-table")
        command(pv_path(0x2D, f"BABE020384{len(panels):04X}0100"))
        command(pv_path(0x22, "0000"))
        command(pv_path(0x41, join_seed))
        command("/api/command/force-learn")

    write(
        "probe_start",
        {
            "duration": args.duration,
            "anchors": sorted(anchors),
            "panels": panels,
            "soft_reset": args.soft_reset,
            "send_join_seed": args.send_join_seed,
        },
    )
    try:
        command("/api/command/hold-learn?confirm=HOLD_LEARN", "POST")
        command(pv_path(0x2B, "BABE"))
        command(pv_path(0x2D, "BABE02038400000100"))
        command(pv_path(0x22, "0001"))
        command(pv_path(0x29, table_body({panel["node_id"] for panel in panels})))
        command("/api/command/node-table")
        command(pv_path(0x2D, "BABE000000000000"))
        write("legacy_table", request("/api/node-map"))

        if args.soft_reset:
            command(
                "/api/command/simple-frame?type=0x0052&expectedType=0x0053"
                "&timeoutMs=3000&wait=true&confirm=UNSAFE_RAW_FRAME",
                "POST",
                timeout=40.0,
            )
            time.sleep(8.0)
            command("/api/command/enumerate?confirm=ENUMERATE_TAP", "POST")

        # This is the compact synchronization block repeatedly emitted by a CCA.
        command(pv_path(0x0D, "0000"))
        command(pv_path(0x0D, "0001"))
        command(pv_path(0x22, "0000"))
        command("/api/command/network-status")
        command("/api/command/node-table")
        command(pv_path(0x22, "0001"))
        if args.send_join_seed:
            command(pv_path(0x41, join_seed))

        observation_started = time.monotonic()
        next_status = observation_started
        next_sync = observation_started + 160.0
        while time.monotonic() - observation_started < args.duration:
            now = time.monotonic()
            if now >= next_status:
                command("/api/command/network-status")
                write(
                    "observation",
                    {"status": status(), "node_map": request("/api/node-map")},
                )
                next_status = now + 10.0
            if now >= next_sync:
                command(pv_path(0x22, "0000"))
                time.sleep(1.0)
                command(pv_path(0x22, "0001"))
                next_sync = now + 160.0
            time.sleep(0.2)
    finally:
        try:
            restore()
            write("restore", {"ok": True, "anchors": sorted(anchors), "status": status()})
        except Exception as exc:
            write("restore", {"ok": False, "error": str(exc), "anchors": sorted(anchors)})
        write("probe_stop", {})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
