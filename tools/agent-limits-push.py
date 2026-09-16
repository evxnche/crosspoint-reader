#!/usr/bin/env python3
"""Push coding-agent usage from CodexBar to the reader's relay.

The reader is rarely on the same network as this Mac, and never at a stable
address, so it cannot fetch from here directly. Instead this pushes a snapshot
to a small always-reachable relay, and the reader fetches that from whatever
Wi-Fi it happens to be on.

Numbers come from the history CodexBar (https://github.com/steipete/codexbar)
keeps per provider. CodexBar has already asked each provider for the real
figure, so these are percentages of the actual plan limit -- not an estimate
rebuilt from token logs. Nothing is requested from any provider here; this only
reads files CodexBar has already written.

Usage:
    python3 tools/agent-limits-push.py --once        # print, do not send
    python3 tools/agent-limits-push.py --push        # send once
    python3 tools/agent-limits-push.py --interval 300

Config lives at ~/Library/Application Support/AgentLimits/config.json:
    {"url": "https://<project>.vercel.app/api/limits", "token": "<PUSH_TOKEN>"}
"""

import argparse
import glob
import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

HISTORY_DIR = os.path.expanduser("~/Library/Application Support/com.steipete.codexbar/history")
CONFIG_PATH = os.path.expanduser("~/Library/Application Support/AgentLimits/config.json")

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

# CodexBar's internal window names, as the reader should label them.
WINDOW_LABELS = {"session": "5-hr", "5h": "5-hr", "weekly": "Weekly", "monthly": "Monthly"}

# Matches AgentLimitsStore's caps.
MAX_PROVIDERS = 12
MAX_WINDOWS = 4


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
    entries = [e for e in window.get("entries", []) if e.get("capturedAt")]
    return max(entries, key=lambda e: e["capturedAt"]) if entries else None


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
    """One provider card: its windows, plus how fresh the reading is.

    Providers with no account and providers last seen weeks ago are still
    returned, carrying a status instead of windows. Dropping them silently is
    what makes the screen look like it is missing providers.
    """
    name = os.path.basename(path)[: -len(".json")]
    label = PROVIDERS.get(name, name.title())
    try:
        with open(path) as handle:
            doc = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"skipping {name}: {exc}", file=sys.stderr)
        return {"name": label, "status": "unreadable", "windows": [], "age": None}

    windows = []
    freshest = None
    stalest_seen = None
    for window in account_windows(doc):
        entry = latest_entry(window)
        if not entry:
            continue
        captured = parse_time(entry.get("capturedAt"))
        if not captured:
            continue
        age = (now - captured).total_seconds()
        stalest_seen = age if stalest_seen is None else min(stalest_seen, age)

        window_name = (window.get("name") or "window").lower()
        resets_at = parse_time(entry.get("resetsAt"))
        windows.append({
            "sort": WINDOW_ORDER.get(window_name, 9),
            "stale": age > max_age_seconds,
            "label": WINDOW_LABELS.get(window_name, window_name.title()),
            "used": int(round(float(entry.get("usedPercent") or 0))),
            "resets": humanize((resets_at - now).total_seconds()) if resets_at else "",
        })
        if freshest is None or age < freshest:
            freshest = age

    if not windows:
        return {"name": label, "status": "not connected", "windows": [], "age": None}

    # Percentages captured weeks ago are not a reading, they are a memory. Keep
    # the provider visible, but say so rather than dressing it up as current.
    if all(w["stale"] for w in windows):
        return {"name": label, "status": f"{humanize(stalest_seen)} old", "windows": [], "age": freshest}

    windows.sort(key=lambda w: w["sort"])
    return {
        "name": label,
        "status": "live" if freshest is not None and freshest <= 300 else f"{humanize(freshest)} old",
        "windows": [{k: w[k] for k in ("label", "used", "resets")} for w in windows[:MAX_WINDOWS]],
        "age": freshest,
    }


def build_payload(max_age_seconds):
    now = datetime.now(timezone.utc)
    if not os.path.isdir(HISTORY_DIR):
        return {"subtitle": "CodexBar not found", "providers": []}

    cards = []
    for path in sorted(glob.glob(os.path.join(HISTORY_DIR, "*.json"))):
        cards.append(read_provider(path, now, max_age_seconds))

    # Configured order first, then anything unrecognised; within that, providers
    # that actually have readings come before the ones that do not.
    priority = {PROVIDERS[key]: index for index, key in enumerate(PROVIDERS)}
    cards.sort(key=lambda c: (0 if c["windows"] else 1, priority.get(c["name"], 99)))

    live = [c["age"] for c in cards if c["age"] is not None]
    subtitle = "CodexBar" if not live else (
        "CodexBar" if min(live) <= 300 else f"CodexBar · {humanize(min(live))} old")

    for card in cards:
        card.pop("age", None)
    return {"subtitle": subtitle, "providers": cards[:MAX_PROVIDERS]}


def load_config(args):
    url, token = args.url, args.token
    if (not url or not token) and os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH) as handle:
                cfg = json.load(handle)
            url = url or cfg.get("url")
            token = token or cfg.get("token")
        except (OSError, json.JSONDecodeError) as exc:
            print(f"cannot read {CONFIG_PATH}: {exc}", file=sys.stderr)
    return url, token


def push(payload, url, token, timeout=15):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, response.read().decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", help="Relay endpoint (default: from config.json).")
    parser.add_argument("--token", help="Push token (default: from config.json).")
    parser.add_argument("--interval", type=float, default=0,
                        help="Seconds between pushes; 0 pushes once and exits.")
    parser.add_argument("--max-age-hours", type=float, default=24,
                        help="Hide a window whose newest sample is older than this.")
    parser.add_argument("--once", action="store_true", help="Print the payload without sending it.")
    args = parser.parse_args()

    max_age_seconds = args.max_age_hours * 3600

    if args.once:
        print(json.dumps(build_payload(max_age_seconds), indent=2))
        return 0

    url, token = load_config(args)
    if not url or not token:
        print(f"No relay configured. Pass --url/--token, or write {CONFIG_PATH}", file=sys.stderr)
        return 1

    while True:
        payload = build_payload(max_age_seconds)
        try:
            status, body = push(payload, url, token)
            print(f"{datetime.now().strftime('%H:%M:%S')} pushed {len(payload['providers'])} providers -> {status} {body}")
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
            # Offline, asleep, captive portal: keep trying rather than exiting,
            # so the agent does not need supervision to recover.
            print(f"{datetime.now().strftime('%H:%M:%S')} push failed: {exc}", file=sys.stderr)

        if args.interval <= 0:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
