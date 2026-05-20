#!/usr/bin/env bash
set -euo pipefail

HOST_IP="${1:-}"
CLIENT_BIN="${2:-$HOME/lan-extended-display/led_client_app}"
CONTROL_PORT="${3:-17660}"
INPUT_PORT="${4:-17691}"
TRAY_SCRIPT="${5:-$HOME/lan-extended-display/led_client_tray.py}"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/lan-extended-display-client.service"

mkdir -p "$SERVICE_DIR"

if [[ -f "$TRAY_SCRIPT" ]]; then
  EXEC_COMMAND="python3 \"${TRAY_SCRIPT}\" --client-bin \"${CLIENT_BIN}\" --default-host \"${HOST_IP}\""
else
  if [[ -z "$HOST_IP" ]]; then
    echo "Usage without tray script: $0 <windows-host-ip> [client-binary] [control-port] [input-port]" >&2
    exit 2
  fi
  EXEC_COMMAND="/bin/bash -lc 'while true; do \"${CLIENT_BIN}\" --receive-test-stream \"${HOST_IP}\" \"${CONTROL_PORT}\" 0 avdec-i420gl 1000 x11-input \"${INPUT_PORT}\"; sleep 1; done'"
fi

cat > "$SERVICE_FILE" <<SERVICE
[Unit]
Description=LAN Extended Display Linux client tray
After=graphical-session.target network-online.target
Wants=network-online.target

[Service]
Type=simple
Environment=DISPLAY=:0
Environment=XAUTHORITY=%h/.Xauthority
ExecStart=${EXEC_COMMAND}
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
SERVICE

systemctl --user daemon-reload
systemctl --user enable lan-extended-display-client.service >/dev/null
systemctl --user restart lan-extended-display-client.service

if command -v loginctl >/dev/null 2>&1; then
  loginctl enable-linger "$USER" >/dev/null 2>&1 || true
fi

echo "Installed and started: $SERVICE_FILE"
echo "Status: systemctl --user status lan-extended-display-client.service"
