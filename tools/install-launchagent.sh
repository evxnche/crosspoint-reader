#!/bin/bash
# Run the agent-limits endpoint automatically at login.
#
# The script is copied out of the repo before being registered: macOS blocks
# background agents from reading ~/Desktop, ~/Documents and ~/Downloads, so a
# LaunchAgent pointed at a checkout in one of those folders fails with
# "Operation not permitted" even though the same command works in a terminal.
#
# Re-run this after changing the server script to install the new version.
set -euo pipefail

PORT="${1:-8765}"
LABEL="com.evan.agentlimits"
SRC="$(cd "$(dirname "$0")" && pwd)/agent-limits-server.py"
DEST_DIR="$HOME/Library/Application Support/AgentLimits"
DEST="$DEST_DIR/agent-limits-server.py"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

mkdir -p "$DEST_DIR" "$HOME/Library/LaunchAgents"
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
    <string>$DEST</string>
    <string>--port</string>
    <string>$PORT</string>
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

echo "Installed. Serving on port $PORT at login."
echo "Reader endpoint: http://$(ipconfig getifaddr en0 2>/dev/null || echo '<your-LAN-IP>'):$PORT/limits"
echo
echo "To remove:  launchctl bootout gui/\$(id -u)/$LABEL && rm '$PLIST'"
