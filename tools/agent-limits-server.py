#!/usr/bin/env python3
"""Serve coding-agent usage to the reader's Agent Limits screen.

The reader fetches one small JSON document over plain HTTP on the local
network. This script produces that document from ccusage, which reads the
Claude Code session logs in ~/.claude.

Usage:
    python3 tools/agent-limits-server.py
    python3 tools/agent-limits-server.py --port 8765 --session-budget 40

Then, on the reader: Settings > System > Agent Limits > Endpoint URL, and
enter http://<this machine's LAN IP>:8765/limits

Budgets are yours to pick. Claude Code does not publish a numeric plan limit,
so the "total" in each row is the ceiling you decide to hold yourself to; set
a budget to 0 to show the raw number with no bar.
"""

import argparse
import json
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# A 5-hour rolling window is what Claude Code meters usage against.
SESSION_WINDOW_MINUTES = 300
# ccusage is a Node CLI; npx fetches it on first use.
CCUSAGE = ["npx", "--yes", "ccusage@latest"]
# Re-running ccusage per request would re-scan every session log.
CACHE_SECONDS = 60


def run_ccusage(args, timeout):
    """Return parsed JSON from a ccusage subcommand, or None on any failure."""
    try:
        result = subprocess.run(
            CCUSAGE + args + ["--json"],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        print(f"ccusage {' '.join(args)} failed: {exc}", file=sys.stderr)
        return None

    if result.returncode != 0:
        print(f"ccusage {' '.join(args)} exited {result.returncode}: {result.stderr.strip()}", file=sys.stderr)
        return None

    # ccusage prints progress lines before the document; start at the first brace.
    start = result.stdout.find("{")
    if start < 0:
        return None
    try:
        return json.loads(result.stdout[start:])
    except json.JSONDecodeError as exc:
        print(f"ccusage {' '.join(args)} returned unparseable JSON: {exc}", file=sys.stderr)
        return None


def humanize_minutes(minutes):
    minutes = max(0, int(minutes))
    hours, mins = divmod(minutes, 60)
    if hours and mins:
        return f"{hours}h {mins}m"
    if hours:
        return f"{hours}h"
    return f"{mins}m"


def money(value):
    """Whole dollars: the reader shows integers, and cents are noise at a glance."""
    return int(round(value))


def build_payload(budgets, timeout):
    limits = []
    subtitle = ""

    blocks = run_ccusage(["blocks", "--active"], timeout)
    active = None
    if blocks:
        for block in blocks.get("blocks", []):
            if block.get("isActive"):
                active = block
                break

    if active:
        remaining = active.get("projection", {}).get("remainingMinutes")
        if remaining is None:
            # Derive it from the window end when ccusage omits the projection.
            end = active.get("endTime")
            try:
                end_dt = datetime.fromisoformat(end.replace("Z", "+00:00"))
                remaining = (end_dt - datetime.now(timezone.utc)).total_seconds() / 60
            except (AttributeError, ValueError):
                remaining = 0
        elapsed = SESSION_WINDOW_MINUTES - max(0, int(remaining))

        limits.append({
            "name": "Session window",
            "used": max(0, elapsed),
            "total": SESSION_WINDOW_MINUTES,
            "unit": "m",
            "resets": humanize_minutes(remaining),
        })
        limits.append({
            "name": "Session spend",
            "used": money(active.get("costUSD", 0)),
            "total": budgets["session"],
            "unit": "$",
            "resets": humanize_minutes(remaining),
        })

        burn = active.get("burnRate", {}).get("costPerHour")
        if burn:
            subtitle = f"${burn:.0f}/hr burn"
    else:
        limits.append({
            "name": "Session window",
            "used": 0,
            "total": SESSION_WINDOW_MINUTES,
            "unit": "m",
            "resets": "idle",
        })

    daily = run_ccusage(["daily"], timeout)
    if daily and daily.get("daily"):
        today = daily["daily"][-1]
        limits.append({
            "name": "Today",
            "used": money(today.get("totalCost", 0)),
            "total": budgets["daily"],
            "unit": "$",
            "resets": "midnight",
        })

    monthly = run_ccusage(["monthly"], timeout)
    if monthly and monthly.get("monthly"):
        this_month = monthly["monthly"][-1]
        limits.append({
            "name": "This month",
            "used": money(this_month.get("totalCost", 0)),
            "total": budgets["monthly"],
            "unit": "$",
            "resets": "1st",
        })

    return {"subtitle": subtitle, "limits": limits}


class LimitsHandler(BaseHTTPRequestHandler):
    budgets = {"session": 0, "daily": 0, "monthly": 0}
    timeout_seconds = 60
    _cache = {"at": 0.0, "body": b""}

    def do_GET(self):
        if self.path.split("?")[0] not in ("/", "/limits"):
            self.send_error(404)
            return

        now = time.time()
        if now - self._cache["at"] > CACHE_SECONDS or not self._cache["body"]:
            payload = build_payload(self.budgets, self.timeout_seconds)
            LimitsHandler._cache = {"at": now, "body": json.dumps(payload).encode()}

        body = self._cache["body"]
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        # The reader has no clock guarantees; let it decide when to refetch.
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print(f"{self.address_string()} {fmt % args}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default: all interfaces).")
    parser.add_argument("--session-budget", type=int, default=50, help="USD ceiling for one 5h window; 0 for none.")
    parser.add_argument("--daily-budget", type=int, default=150, help="USD ceiling for a day; 0 for none.")
    parser.add_argument("--monthly-budget", type=int, default=2000, help="USD ceiling for a month; 0 for none.")
    parser.add_argument("--ccusage-timeout", type=int, default=60, help="Seconds to wait for each ccusage call.")
    parser.add_argument("--once", action="store_true", help="Print the JSON and exit, without serving.")
    args = parser.parse_args()

    budgets = {
        "session": args.session_budget,
        "daily": args.daily_budget,
        "monthly": args.monthly_budget,
    }

    if not shutil.which("npx"):
        print("npx not found; install Node.js so ccusage can run.", file=sys.stderr)
        return 1

    if args.once:
        print(json.dumps(build_payload(budgets, args.ccusage_timeout), indent=2))
        return 0

    LimitsHandler.budgets = budgets
    LimitsHandler.timeout_seconds = args.ccusage_timeout
    server = ThreadingHTTPServer((args.host, args.port), LimitsHandler)
    print(f"Serving agent limits on http://{args.host}:{args.port}/limits")
    print("Point the reader at http://<this machine's LAN IP>:%d/limits" % args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
