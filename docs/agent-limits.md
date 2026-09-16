# Agent Limits

**Settings → System → Agent Limits** shows how much of each coding-agent usage
window is spent, on the reader's screen.

E-ink holds an image with no power, so the numbers stay readable on a device
sitting on the desk next to you — which is the point. The reader never
guesses at them: it fetches from a URL you control.

## Setup

### 1. Run the endpoint on your machine

    python3 tools/agent-limits-server.py

It prints the port it is listening on (8765 by default). The script reads your
Claude Code session logs through [ccusage](https://github.com/ryoppippi/ccusage)
and needs Node installed for `npx`.

Check it first without serving:

    python3 tools/agent-limits-server.py --once

### 2. Find your machine's LAN address

    ipconfig getifaddr en0      # macOS, Wi-Fi

### 3. Point the reader at it

On the device: **Settings → System → Agent Limits → Endpoint URL**, and enter

    http://192.168.1.42:8765/limits

Then choose **Refresh now**. The reader connects to Wi-Fi, fetches once, saves
the result, and disconnects.

## How refreshing works

Fetching is manual, never automatic. Bringing up the radio costs seconds and
battery, and a reader should not do that on its own.

The last values are stored on the card, so the screen paints instantly from the
cache when you open it — with the age of the numbers shown next to them, so you
always know how much to trust what you are looking at.

## The response format

Any server that returns this shape works; ccusage is just the default source.

```json
{
  "subtitle": "$17/hr burn",
  "limits": [
    { "name": "Session window", "used": 104, "total": 300,  "unit": "m", "resets": "3h 16m" },
    { "name": "Session spend",  "used": 25,  "total": 50,   "unit": "$", "resets": "3h 16m" },
    { "name": "Today",          "used": 44,  "total": 150,  "unit": "$", "resets": "midnight" },
    { "name": "This month",     "used": 279, "total": 2000, "unit": "$", "resets": "1st" }
  ]
}
```

| Field | Meaning |
|---|---|
| `name` | Row label. Up to 40 characters. |
| `used` | The number shown first. Whole numbers only. |
| `total` | The cap. **0 shows `used` alone**, with no ratio. |
| `unit` | Printed straight after the numbers: `%`, `$`, `m`, `req`. |
| `resets` | Free text shown after the numbers, e.g. `3h 12m`. |
| `subtitle` | Optional status line above the refresh row. |

At most 6 rows are kept. A response that parses but lists no usable row leaves
the previous values in place rather than blanking the screen.

## A note on the "limits"

Claude Code does not publish a numeric plan limit, so `total` is whatever
ceiling **you** decide to hold yourself to. Set your own with

    python3 tools/agent-limits-server.py --session-budget 40 --daily-budget 120

or pass `0` for a row you would rather just watch than bound.

The one genuinely fixed number here is the session window: usage is metered
against a rolling 5 hours, and "Session window 104/300m" is really telling you
how much of that window has elapsed.

## Security

The endpoint is plain HTTP on your local network and carries your usage figures.
Do not expose it to the internet — bind it to the LAN, which is what the default
does behind a home router. HTTPS URLs work too if you put the endpoint behind a
TLS terminator.
