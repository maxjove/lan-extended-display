#!/usr/bin/env bash
set -euo pipefail

DISPLAY_NUMBER="${LED_WEB_DISPLAY:-:10}"
GEOMETRY="${LED_WEB_GEOMETRY:-1600x900x24}"
VNC_PORT="${LED_WEB_VNC_PORT:-5901}"
WEB_PORT="${LED_WEB_PORT:-6080}"
NOVNC_DIR="${LED_NOVNC_DIR:-/usr/share/novnc}"
SESSION="${LED_WEB_SESSION:-dde}"
PANEL="${LED_WEB_PANEL:-lxpanel}"
START_TERMINAL="${LED_WEB_START_TERMINAL:-auto}"
CONTROL_TRAY="${LED_WEB_CONTROL_TRAY:-1}"
CONTROL_TRAY_SCRIPT="${LED_WEB_CONTROL_TRAY_SCRIPT:-$HOME/lan-extended-display/led_web_control_tray.py}"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/lan-extended-display/web-desktop"

mkdir -p "$STATE_DIR"

XVFB_PID=""
SESSION_PID=""
PANEL_PID=""
CONTROL_TRAY_PID=""
XTERM_PID=""
X11VNC_PID=""
WEBSOCKIFY_PID=""

cleanup() {
  for pid in "$WEBSOCKIFY_PID" "$X11VNC_PID" "$XTERM_PID" "$CONTROL_TRAY_PID" "$PANEL_PID" "$SESSION_PID" "$XVFB_PID"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  if [[ -n "$SESSION_PID" ]]; then
    pkill -P "$SESSION_PID" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT INT TERM

if [[ ! -d "$NOVNC_DIR" ]]; then
  echo "noVNC directory not found: $NOVNC_DIR" >&2
  exit 2
fi

Xvfb "$DISPLAY_NUMBER" -screen 0 "$GEOMETRY" -nolisten tcp >"$STATE_DIR/xvfb.log" 2>&1 &
XVFB_PID="$!"

for _ in $(seq 1 40); do
  if DISPLAY="$DISPLAY_NUMBER" xdpyinfo >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

if ! DISPLAY="$DISPLAY_NUMBER" xdpyinfo >/dev/null 2>&1; then
  echo "Xvfb did not become ready on ${DISPLAY_NUMBER}" >&2
  sed -n '1,120p' "$STATE_DIR/xvfb.log" >&2 || true
  exit 1
fi

if [[ "$SESSION" == "dde" ]] && command -v startdde >/dev/null 2>&1 && command -v dbus-run-session >/dev/null 2>&1; then
  env -u DBUS_SESSION_BUS_ADDRESS DISPLAY="$DISPLAY_NUMBER" dbus-run-session startdde >"$STATE_DIR/session.log" 2>&1 &
  SESSION_PID="$!"
  if [[ "$START_TERMINAL" == "auto" ]]; then
    START_TERMINAL="0"
  fi
else
  DISPLAY="$DISPLAY_NUMBER" openbox >"$STATE_DIR/session.log" 2>&1 &
  SESSION_PID="$!"
  if [[ "$START_TERMINAL" == "auto" ]]; then
    START_TERMINAL="1"
  fi
fi

if [[ "$PANEL" == "lxpanel" ]] && command -v lxpanel >/dev/null 2>&1; then
  mkdir -p "$HOME/.config/lxpanel/LXDE/panels"
  cat > "$HOME/.config/lxpanel/LXDE/config" <<'LXCONFIG'
[Command]
FileManager=dde-file-manager
Terminal=x-terminal-emulator
Logout=
LXCONFIG
  cat > "$HOME/.config/lxpanel/LXDE/panels/panel" <<'LXPANEL'
Global {
    edge=bottom
    allign=left
    margin=0
    widthtype=percent
    width=100
    height=44
    transparent=0
    tintcolor=#23272e
    alpha=255
    setdocktype=1
    setpartialstrut=1
    usefontcolor=1
    fontcolor=#f4f6f8
    usefontsize=1
    fontsize=11
    background=0
}

Plugin {
    type = space
    Config {
        Size=6
    }
}

Plugin {
    type = menu
    Config {
        image=/usr/share/lxpanel/images/my-computer.png
        system {
        }
        separator {
        }
        item {
            command=run
        }
        item {
            image=gnome-logout
            command=logout
        }
    }
}

Plugin {
    type = space
    Config {
        Size=8
    }
}

Plugin {
    type = taskbar
    expand=1
    Config {
        tooltips=1
        IconsOnly=0
        AcceptSkipPager=1
        ShowIconified=1
        ShowMapped=1
        ShowAllDesks=1
        UseMouseWheel=1
        UseUrgencyHint=1
        FlatButton=0
        MaxTaskWidth=220
        spacing=4
    }
}

Plugin {
    type = tray
}

Plugin {
    type = space
    Config {
        Size=8
    }
}

Plugin {
    type = dclock
    Config {
        ClockFmt=%H:%M
        TooltipFmt=%Y-%m-%d %A
        BoldFont=0
    }
}

Plugin {
    type = space
    Config {
        Size=6
    }
}
LXPANEL
  DISPLAY="$DISPLAY_NUMBER" lxpanel --profile LXDE >"$STATE_DIR/lxpanel.log" 2>&1 &
  PANEL_PID="$!"
fi

if [[ "$CONTROL_TRAY" == "1" && -f "$CONTROL_TRAY_SCRIPT" ]]; then
  DISPLAY="$DISPLAY_NUMBER" python3 "$CONTROL_TRAY_SCRIPT" >"$STATE_DIR/control-tray.log" 2>&1 &
  CONTROL_TRAY_PID="$!"
fi

if [[ "$START_TERMINAL" == "1" ]] && command -v xterm >/dev/null 2>&1; then
  DISPLAY="$DISPLAY_NUMBER" xterm -geometry 100x30+40+40 -title "LAN Extended Display Web Desktop" >"$STATE_DIR/xterm.log" 2>&1 &
  XTERM_PID="$!"
fi

x11vnc \
  -display "$DISPLAY_NUMBER" \
  -rfbport "$VNC_PORT" \
  -localhost \
  -forever \
  -shared \
  -nopw \
  -quiet \
  >"$STATE_DIR/x11vnc.log" 2>&1 &
X11VNC_PID="$!"

websockify \
  --web "$NOVNC_DIR" \
  "0.0.0.0:${WEB_PORT}" \
  "127.0.0.1:${VNC_PORT}" \
  >"$STATE_DIR/websockify.log" 2>&1 &
WEBSOCKIFY_PID="$!"

echo "LAN Extended Display web desktop is running"
echo "URL: http://$(hostname -I | awk '{print $1}'):${WEB_PORT}/vnc.html"
echo "Display: ${DISPLAY_NUMBER}"
echo "Session: ${SESSION}"
echo "Panel: ${PANEL}"
echo "VNC: 127.0.0.1:${VNC_PORT}"

wait "$WEBSOCKIFY_PID"
