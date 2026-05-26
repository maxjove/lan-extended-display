#!/usr/bin/env bash
set -euo pipefail

SERVICE_FILE="$HOME/.config/systemd/user/lan-extended-display-web-desktop.service"

systemctl --user disable --now lan-extended-display-web-desktop.service >/dev/null 2>&1 || true
rm -f "$SERVICE_FILE"
systemctl --user daemon-reload

echo "Removed LAN Extended Display web desktop service."
