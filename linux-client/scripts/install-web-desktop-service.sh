#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
START_SCRIPT="${SCRIPT_DIR}/start-web-desktop.sh"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/lan-extended-display-web-desktop.service"

if [[ ! -x "$START_SCRIPT" ]]; then
  echo "Missing executable start script: $START_SCRIPT" >&2
  echo "Run: chmod +x $START_SCRIPT" >&2
  exit 2
fi

mkdir -p "$SERVICE_DIR"

cat > "$SERVICE_FILE" <<SERVICE
[Unit]
Description=LAN Extended Display browser-accessible Linux desktop
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=${START_SCRIPT}
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
SERVICE

systemctl --user daemon-reload
systemctl --user enable lan-extended-display-web-desktop.service >/dev/null
systemctl --user restart lan-extended-display-web-desktop.service

if command -v loginctl >/dev/null 2>&1; then
  loginctl enable-linger "$USER" >/dev/null 2>&1 || true
fi

echo "Installed and started: $SERVICE_FILE"
echo "URL: http://$(hostname -I | awk '{print $1}'):6080/vnc.html"
echo "Status: systemctl --user status lan-extended-display-web-desktop.service"
