#!/bin/bash
# Keep the reader's usage numbers up to date, automatically.
#
# Installs agent-limits-push.py as a login agent that pushes a CodexBar snapshot
# to the relay every few minutes. The script is copied out of the repo first:
# macOS denies a background agent read access to ~/Desktop, ~/Documents and
# ~/Downloads, so a LaunchAgent pointed at a checkout in one of those folders
# dies with "Operation not permitted".
#
# Usage:
#   ./tools/install-launchagent.sh <relay-url> <push-token> [interval-seconds]
#
# Re-run it after changing the push script to install the new version.
set -euo pipefail

URL="${1:-}"
TOKEN="${2:-}"
INTERVAL="${3:-300}"
LABEL="com.evan.agentlimits"
SRC="$(cd "$(dirname "$0")" && pwd)/agent-limits-push.py"
DEST_DIR="$HOME/Library/Application Support/AgentLimits"
DEST="$DEST_DIR/agent-limits-push.py"
CONFIG="$DEST_DIR/config.json"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

mkdir -p "$DEST_DIR" "$HOME/Library/LaunchAgents"

if [ -n "$URL" ] && [ -n "$TOKEN" ]; then
  # 600: the push token is a write credential for the relay.
  umask 077
  printf '{\n  "url": %s,\n  "token": %s\n}\n' "\"$URL\"" "\"$TOKEN\"" > "$CONFIG"
  umask 022
elif [ ! -f "$CONFIG" ]; then
  echo "No relay configured. Pass the URL and push token:" >&2
  echo "  $0 https://<project>.vercel.app/api/limits <PUSH_TOKEN>" >&2
  exit 1
fi

cp "$SRC" "$DEST"

cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/bin/python3</string>
    <string>-u</string>
    <string>$DEST</string>
    <string>--interval</string>
    <string>$INTERVAL</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/tmp/agentlimits.log</string>
  <key>StandardErrorPath</key><string>/tmp/agentlimits.err</string>
</dict>
</plist>
PLIST_EOF

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"

echo "Installed. Pushing every ${INTERVAL}s, starting at login."
echo "Logs: /tmp/agentlimits.log  (errors: /tmp/agentlimits.err)"
echo
echo "To remove:  launchctl bootout gui/\$(id -u)/$LABEL && rm '$PLIST'"
