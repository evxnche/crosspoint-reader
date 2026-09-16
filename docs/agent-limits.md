# Agent Limits

**Agent Limits**, on the home screen (and under Settings → System), shows how
much of each coding-agent usage window is spent.

E-ink holds an image with no power, so the numbers stay readable on a device
sitting on the desk next to you — which is the point. The reader never
guesses at them: it fetches from a URL you control.

## How the pieces fit

The reader has no account with any AI provider, and no way to ask one how much
quota is left. This Mac can, because CodexBar already tracks it. But the two are
rarely on the same Wi-Fi and the Mac has no fixed address, so the reader cannot
simply call it.

So the Mac pushes, and the reader pulls, with a small relay in between:

    Mac (anywhere)                       Reader (anywhere)
       |  POST every 5 min                  |  GET on Refresh
       v                                    v
    +-------------------------------------------+
    |  https://<project>.vercel.app/api/limits  |
    +-------------------------------------------+

Because both sides reach the relay over the internet, neither needs to know
where the other is. Work Wi-Fi, a cafe, a phone hotspot — it makes no
difference, and it keeps working while the Mac is shut in a bag. The reader then
shows the numbers with their real age, so a stale reading is never mistaken for
a current one.

## Setup

### 1. Deploy the relay

`relay/` holds a single Vercel function that stores the most recent snapshot and
hands it back. Deploy it, then set three environment variables on the project:

| Variable | What it is |
|---|---|
| `PUSH_TOKEN` | Secret the Mac sends to write. Generate a long random string. |
| `READ_KEY` | Secret the reader sends to read. Travels in the URL. |
| `BLOB_PATH_SECRET` | Random segment in the stored object's path. |

`BLOB_PATH_SECRET` exists because the Blob store is public: the SDK version in
use cannot read a private blob, so an unguessable path is what keeps the raw
object from being found directly. The endpoint itself is key-gated either way.

### 2. Start pushing from the Mac

    ./tools/install-launchagent.sh https://<project>.vercel.app/api/limits <PUSH_TOKEN>

That installs a login agent which pushes every 5 minutes, and keeps running
across reboots. Check it with:

    tail -f /tmp/agentlimits.log

The script is copied to `~/Library/Application Support/AgentLimits/` before
being registered. The copy is deliberate: macOS denies a background agent read
access to `~/Desktop`, `~/Documents` and `~/Downloads`, so a LaunchAgent pointed
straight at a checkout in one of those folders dies with "Operation not
permitted". Re-run the installer after changing the script.

    # to stop it again
    launchctl bootout gui/$(id -u)/com.evan.agentlimits
    rm ~/Library/LaunchAgents/com.evan.agentlimits.plist

To see what would be sent, without sending it:

    python3 tools/agent-limits-push.py --once

### 3. Point the reader at it

On the device: **Agent Limits → Endpoint URL**, and enter

    https://<project>.vercel.app/api/limits?k=<READ_KEY>

Typed once. After that, **Refresh now** is the only control.

## How refreshing works

Fetching is manual, never automatic. Bringing up the radio costs seconds and
battery, and a reader should not do that on its own. The Mac's pushing is the
automatic half; the reader only catches up when asked.

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

Nothing in the firmware knows about CodexBar, Vercel, or any of this. The reader
fetches a URL and renders the JSON it gets back, so any server returning that
shape works — swapping the source means replacing the push script, or just
pointing the Endpoint URL somewhere else.

`total` is always 100 here because CodexBar reports a percentage of the plan
limit. A source with no notion of a cap can send `total: 0`, and the reader
shows the bare number instead of a ratio.

## Security

Both hops are HTTPS, and both are authenticated: `PUSH_TOKEN` to write,
`READ_KEY` to read. Writes are rejected unless the body parses and contains a
`limits` array, and are capped at 16KB.

The read key travels in the query string because the reader cannot set request
headers. Treat that URL as the secret it is — anyone holding it can see your
usage percentages, though not do anything with them. Rotate by changing
`READ_KEY` on the Vercel project and re-entering the URL on the device.
