#!/usr/bin/env bash
set -euo pipefail

SERVICE_FILE="$HOME/.config/systemd/user/lan-extended-display-client.service"

systemctl --user disable --now lan-extended-display-client.service >/dev/null 2>&1 || true
rm -f "$SERVICE_FILE"
systemctl --user daemon-reload

echo "Removed LAN Extended Display user autostart service."
