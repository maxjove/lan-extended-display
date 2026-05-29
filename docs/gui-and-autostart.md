# GUI and Autostart

## Windows Host Tray

Build:

```bat
cmake --build build --config Release --target led_host_app led_host_tray
```

Run:

```powershell
.\build\windows-host\Release\led_host_tray.exe
```

The tray icon menu provides:

- `Start default`: starts `led_host_app.exe --serve-mjpeg-capture 17660 17670 0 60 55 1920 1080 17691 sendinput` by default
- `Start discovered client`: starts a Linux client discovered on the LAN through UDP port `17659`
- `Start by IP...`: sends a connect command to a manually entered Linux client IP, then starts the extended display
- `Refresh clients`: broadcasts a discovery probe on UDP port `17659`
- `Quality`: selects MJPEG quality: `Fast 55`, `Balanced 75`, `Sharp 85`, or `Ultra 90`. The value is saved in `led_host_tray.ini`; if a display session is running, the tray restarts that session automatically so the new quality takes effect.
- `Stop extended display`: asks the running host to stop through `Local\LanExtendedDisplayHostStop`, then asks the IddCx driver to detach the virtual monitor without requiring administrator confirmation
- `Install firewall rules`: asks for administrator permission and opens inbound TCP `17660` plus UDP `17659`, `17670`, and `17691` on private/domain networks
- `Open log`: opens `led_host_tray.log`
- `Exit`: stops the host if it was started by the tray app, detaches the virtual monitor, then exits the tray app

Keep `led_host_tray.exe` and `led_host_app.exe` in the same directory.
The driver scripts are resolved from the source tree, so the current development layout expects the tray app under `build\windows-host\Release`.

## LAN Discovery

Linux clients advertise themselves with UDP `LED_CLIENT_V1` beacons on port `17659`.
Windows sends `LED_DISCOVER_V1` probes and can send `LED_CONNECT_V1` to ask a selected Linux client to connect back to the Windows host.
If discovered clients do not appear, run `Install firewall rules` from the Windows tray menu once.

The video/control path is unchanged: Windows listens on TCP `17660`, sends low-latency MJPEG video on UDP `17670`, and receives input on UDP `17691`.
Frame latency telemetry uses UDP `17692`. The host logs `receive_ack_*` and `render_ack_*`:

- `receive_ack_*`: Windows frame send to Linux RTP receive/decode-queue acknowledgement RTT
- `render_ack_*`: Windows frame send to Linux decoded-frame renderer submission RTT

Both are clock-safe because they use the Windows send timestamp and Windows receive timestamp around a Linux ACK.

The virtual display is session-scoped: it should not be present before `Start extended display`.
If no Linux client connects during an initial manual start, the tray app removes the virtual display automatically.
After a session has been established for a selected Linux IP, the tray arms automatic recovery:

- If the selected Linux client disappears from discovery, Windows keeps the tray session armed and periodically sends a lightweight recovery connect command.
- If the client later reappears on the LAN, Windows sends a `LED_CONNECT_V1` command with `reason=recover`.
- If `led_host_app.exe` exits because the control connection or frame ACKs were interrupted, the tray starts a recovery loop instead of immediately giving up. It recreates the virtual monitor, restarts the host process, and asks the Linux tray to reconnect.
- Manual `Stop extended display` disables recovery and removes the virtual display immediately.

## Linux Tray

Install the Linux user service with:

```bash
./linux-client/scripts/install-user-autostart.sh [windows-host-ip] ~/lan-extended-display/led_client_app 17660 17691 ~/lan-extended-display/led_client_tray.py
```

When `led_client_tray.py` is present, the service runs the tray/discovery helper. The tray tooltip shows:

- service status
- current Linux IP addresses
- connected Windows host

If the desktop does not provide GTK tray support, the helper continues running discovery/control in console mode under systemd.

## Linux Client Autostart

Install the built client binary in a stable path first, for example:

```bash
mkdir -p "$HOME/lan-extended-display"
cp /path/to/led_client_app "$HOME/lan-extended-display/led_client_app"
chmod +x "$HOME/lan-extended-display/led_client_app"
```

Install and start the user service. The service loops automatically while the Windows host is offline; failed TCP connects time out quickly and retry once per second.

```bash
bash linux-client/scripts/install-user-autostart.sh <windows-host-ip> "$HOME/lan-extended-display/led_client_app"
```

Example:

```bash
bash linux-client/scripts/install-user-autostart.sh 10.168.20.134 "$HOME/lan-extended-display/led_client_app"
```

Check status:

```bash
systemctl --user status lan-extended-display-client.service
journalctl --user -u lan-extended-display-client.service -f
```

Remove autostart:

```bash
bash linux-client/scripts/uninstall-user-autostart.sh
```
