#!/usr/bin/env python3
"""Serve coding-agent usage to the reader's Agent Limits screen.

Reads the history CodexBar (https://github.com/steipete/codexbar) keeps for each
provider and serves it as the small JSON document the reader fetches.

CodexBar already asks each provider for the real figure, so the percentages here
are usage against the actual plan limit -- not an estimate reconstructed from
token logs. Nothing is requested from any provider by this script; it only reads
files CodexBar has already written.

Usage:
    python3 tools/agent-limits-server.py
    python3 tools/agent-limits-server.py --once
    python3 tools/agent-limits-server.py --port 8765 --max-age-hours 6

Then, on the reader: Settings > System > Agent Limits > Endpoint URL, and enter
http://<this machine's LAN IP>:8765/limits
"""

import argparse
import glob
import json
import os
import sys
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HISTORY_DIR = os.path.expanduser("~/Library/Application Support/com.steipete.codexbar/history")

# Display names, in the order they should appear on the reader. Providers absent
# from this map still show, under their filename.
PROVIDERS = {
    "claude": "Claude",
    "codex": "Codex",
    "cursor": "Cursor",
    "muse": "Muse",
    "antigravity": "Antigravity",
    "zai": "Z.ai",
    "opencodego": "OpenCode",
    "commandcode": "CommandCode",
}

# Shorter windows first: the one about to run out is the one worth seeing.
WINDOW_ORDER = {"session": 0, "5h": 0, "weekly": 1, "week": 1, "monthly": 2, "month": 2}

# The reader keeps six rows.
MAX_ROWS = 6
# Re-reading every request would re-parse several hundred KB of history.
CACHE_SECONDS = 30


def parse_time(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


def humanize(delta_seconds):
    """A short, glanceable duration: '3h 2m', '4d 15h', 'now'."""
    seconds = int(delta_seconds)
    if seconds <= 0:
        return "now"
    days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600)
    minutes = seconds // 60
    if days:
        return f"{days}d {hours}h" if hours else f"{days}d"
    if hours:
        return f"{hours}h {minutes}m" if minutes else f"{hours}h"
    return f"{minutes}m"


def latest_entry(window):
    """Newest sample in a window, by capture time."""
    entries = [e for e in window.get("entries", []) if e.get("capturedAt")]
    if not entries:
        return None
    return max(entries, key=lambda e: e["capturedAt"])


def account_windows(doc):
    """Windows for the account CodexBar prefers, else the most recently seen."""
    accounts = doc.get("accounts") or {}
    if not accounts:
        return []

    preferred = doc.get("preferredAccountKey")
    if preferred in accounts:
        return accounts[preferred]

    def newest(key):
        stamps = [e.get("capturedAt", "") for w in accounts[key] for e in w.get("entries", [])]
        return max(stamps, default="")

    return accounts[max(accounts, key=newest)]


def read_provider(path, now, max_age_seconds):
    """Rows for one provider file, newest sample per window."""
    name = os.path.basename(path)[: -len(".json")]
    label = PROVIDERS.get(name, name.title())
    try:
        with open(path) as handle:
            doc = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"skipping {name}: {exc}", file=sys.stderr)
        return []

    rows = []
    for window in account_windows(doc):
        entry = latest_entry(window)
        if not entry:
            continue
        captured = parse_time(entry.get("capturedAt"))
        if not captured:
            continue
        age = (now - captured).total_seconds()
        # A provider signed out months ago should not sit on the screen looking
        # like a live reading.
        if age > max_age_seconds:
            continue

        window_name = (window.get("name") or "window").lower()
        resets_at = parse_time(entry.get("resetsAt"))
        resets = humanize((resets_at - now).total_seconds()) if resets_at else ""

        rows.append({
            "sort": (WINDOW_ORDER.get(window_name, 9), name),
            "age": age,
            "row": {
                "name": f"{label} {window_name}",
                "used": int(round(float(entry.get("usedPercent") or 0))),
                "total": 100,
                "unit": "%",
                "resets": resets,
            },
        })
    return rows


def build_payload(max_age_seconds):
    now = datetime.now(timezone.utc)
    if not os.path.isdir(HISTORY_DIR):
        return {"subtitle": "CodexBar not found", "limits": []}

    collected = []
    for path in sorted(glob.glob(os.path.join(HISTORY_DIR, "*.json"))):
        collected += read_provider(path, now, max_age_seconds)

    if not collected:
        return {"subtitle": "No fresh usage data", "limits": []}

    # Providers in the configured order, shortest window first within each.
    priority = {key: index for index, key in enumerate(PROVIDERS)}
    collected.sort(key=lambda item: (priority.get(item["sort"][1], 99), item["sort"][0]))

    freshest = min(item["age"] for item in collected)
    subtitle = f"CodexBar · {humanize(freshest)} ago" if freshest > 60 else "CodexBar · live"

    return {"subtitle": subtitle, "limits": [item["row"] for item in collected[:MAX_ROWS]]}


class LimitsHandler(BaseHTTPRequestHandler):
    max_age_seconds = 24 * 3600
    _cache = {"at": 0.0, "body": b""}

    def do_GET(self):
        if self.path.split("?")[0] not in ("/", "/limits"):
            self.send_error(404)
            return

        now = time.time()
        if now - self._cache["at"] > CACHE_SECONDS or not self._cache["body"]:
            payload = build_payload(self.max_age_seconds)
            LimitsHandler._cache = {"at": now, "body": json.dumps(payload).encode()}

        body = self._cache["body"]
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print(f"{self.address_string()} {fmt % args}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default: all interfaces).")
    parser.add_argument("--max-age-hours", type=float, default=24,
                        help="Hide a window whose newest sample is older than this.")
    parser.add_argument("--once", action="store_true", help="Print the JSON and exit, without serving.")
    args = parser.parse_args()

    max_age_seconds = args.max_age_hours * 3600

    if not os.path.isdir(HISTORY_DIR):
        print(f"CodexBar history not found at {HISTORY_DIR}", file=sys.stderr)
        print("Install and run CodexBar, or point HISTORY_DIR at its history folder.", file=sys.stderr)
        return 1

    if args.once:
        print(json.dumps(build_payload(max_age_seconds), indent=2))
        return 0

    LimitsHandler.max_age_seconds = max_age_seconds
    server = ThreadingHTTPServer((args.host, args.port), LimitsHandler)
    print(f"Serving agent limits on http://{args.host}:{args.port}/limits")
    print(f"Point the reader at http://<this machine's LAN IP>:{args.port}/limits")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
