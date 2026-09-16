# Agent Limits

**Agent Limits**, on the home screen (and under Settings → System), shows how
much of each coding-agent usage window is spent.

E-ink holds an image with no power, so the numbers stay readable on a device
sitting on the desk next to you — which is the point. The reader never
guesses at them: it fetches from a URL you control.

## Setup

### 1. Run the endpoint on your machine

    python3 tools/agent-limits-server.py

It prints the port it is listening on (8765 by default).

The script reads the per-provider history that
[CodexBar](https://github.com/steipete/codexbar) keeps in
`~/Library/Application Support/com.steipete.codexbar/history/`. CodexBar has
already asked each provider for the real figure, so these are percentages of
your **actual plan limit** — not an estimate rebuilt from token logs. This
script only reads files that already exist; it contacts no provider itself, and
needs CodexBar installed and running.

Windows whose newest sample is over 24 hours old are hidden, so a provider you
signed out of months ago does not sit on screen looking live. Change that with
`--max-age-hours`.

Check it first without serving:

    python3 tools/agent-limits-server.py --once

To have it start at login instead of running it by hand:

    ./tools/install-launchagent.sh

That copies the script to `~/Library/Application Support/AgentLimits/` before
registering it. The copy is deliberate: macOS refuses a background agent read
access to `~/Desktop`, `~/Documents` and `~/Downloads`, so a LaunchAgent pointed
straight at a checkout in one of those folders dies with "Operation not
permitted". Re-run the installer after changing the script.

    # to remove it again
    launchctl bootout gui/$(id -u)/com.evan.agentlimits
    rm ~/Library/LaunchAgents/com.evan.agentlimits.plist

### 2. Find your machine's LAN address

    ipconfig getifaddr en0      # macOS, Wi-Fi

### 3. Point the reader at it

On the device: **Agent Limits → Endpoint URL**, and enter

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

Any server that returns this shape works; CodexBar is just where the bundled
script happens to read from.

```json
{
  "subtitle": "CodexBar · live",
  "limits": [
    { "name": "Claude session", "used": 48, "total": 100, "unit": "%", "resets": "3h 1m" },
    { "name": "Claude weekly",  "used": 13, "total": 100, "unit": "%", "resets": "4d 14h" },
    { "name": "Codex session",  "used": 2,  "total": 100, "unit": "%", "resets": "3h 6m" },
    { "name": "Codex weekly",   "used": 56, "total": 100, "unit": "%", "resets": "2d 13h" }
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

## Using a different source

Nothing in the firmware knows about CodexBar. The reader consumes the JSON above
and nothing else, so pointing it at a different source means replacing this one
script — or pointing the Endpoint URL at something else entirely.

`total` is always 100 here because CodexBar reports a percentage of the plan
limit. A source with no notion of a cap can send `total: 0`, and the reader
shows the bare number instead of a ratio.

## Security

The endpoint is plain HTTP on your local network and carries your usage figures.
Do not expose it to the internet — bind it to the LAN, which is what the default
does behind a home router. HTTPS URLs work too if you put the endpoint behind a
TLS terminator.
