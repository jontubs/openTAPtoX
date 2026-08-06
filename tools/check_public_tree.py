#!/usr/bin/env python3
"""Reject tracked data that commonly identifies a private installation."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_PATH_PREFIXES = (
    "data/",
    "docs/analysis/",
    "docs/handover_",
    "docs/tigo_panel_",
    "assets/Screenshot ",
)

FORBIDDEN_SUFFIXES = (
    ".pcap",
    ".pcapng",
    ".raw",
)

PATTERNS = (
    (
        "absolute home-directory path",
        re.compile(rb"(?i)(?:[a-z]:[\\/]+users[\\/]+[^\\/\s\"']+|/(?:home|users)/[^/\s\"']+)"),
    ),
    (
        "private IPv4 address",
        re.compile(
            rb"(?<!\d)(?:10(?:\.\d{1,3}){3}|192\.168(?:\.\d{1,3}){2}|"
            rb"172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2})(?!\d)"
        ),
    ),
    (
        "hardware EUI-64",
        re.compile(rb"(?i)(?<![0-9a-f])04c05b[34][0-9a-f]{9}(?![0-9a-f])"),
    ),
    (
        "MAC address",
        re.compile(rb"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])"),
    ),
)

EMAIL_PATTERN = re.compile(
    rb"(?i)[a-z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-z0-9-]+"
    rb"(?:\.[a-z0-9-]+)*\.[a-z]{2,63}"
)
SAFE_EMAIL_SUFFIXES = (
    b"@example.com",
    b"@example.org",
    b"@users.noreply.github.com",
    b"@noreply.github.com",
    b".invalid",
)


def git_output(*args: str) -> bytes:
    return subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout


def tracked_paths() -> list[str]:
    raw = git_output("ls-files", "-z")
    return [item.decode("utf-8") for item in raw.split(b"\0") if item]


def line_number(data: bytes, offset: int) -> int:
    return data.count(b"\n", 0, offset) + 1


def main() -> int:
    violations: list[str] = []

    for relative in tracked_paths():
        normalized = relative.replace("\\", "/")
        if normalized.startswith(FORBIDDEN_PATH_PREFIXES):
            violations.append(f"{relative}: forbidden tracked diagnostic path")
            continue
        if normalized.lower().endswith(FORBIDDEN_SUFFIXES):
            violations.append(f"{relative}: forbidden raw-capture extension")

        path = REPO_ROOT / relative
        try:
            data = path.read_bytes()
        except OSError as exc:
            violations.append(f"{relative}: cannot read tracked file: {exc}")
            continue

        if b"\0" in data:
            continue

        for label, pattern in PATTERNS:
            for match in pattern.finditer(data):
                violations.append(
                    f"{relative}:{line_number(data, match.start())}: {label}"
                )

        for match in EMAIL_PATTERN.finditer(data):
            value = match.group(0).lower()
            if not value.endswith(SAFE_EMAIL_SUFFIXES):
                violations.append(
                    f"{relative}:{line_number(data, match.start())}: personal email address"
                )

    author_emails = git_output("log", "--format=%ae").splitlines()
    for email in author_emails:
        value = email.strip().lower()
        if value and not value.endswith(SAFE_EMAIL_SUFFIXES):
            violations.append("Git history: personal author email address")
            break

    if violations:
        print("public-tree privacy check failed:", file=sys.stderr)
        for violation in violations:
            print(f"- {violation}", file=sys.stderr)
        return 1

    print(f"public-tree privacy check OK ({len(tracked_paths())} tracked files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
