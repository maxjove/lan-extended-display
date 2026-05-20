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

- `Start default`: starts `led_host_app.exe --serve-live-capture 17660 17670 17691 0 60 sendinput 20000 1920 1080`
- `Start discovered client`: starts a Linux client discovered on the LAN through UDP port `17659`
- `Start by IP...`: sends a connect command to a manually entered Linux client IP, then starts the extended display
- `Refresh clients`: broadcasts a discovery probe on UDP port `17659`
- `Stop extended display`: asks the running host to stop through `Local\LanExtendedDisplayHostStop`, then removes the IddCx root display device so it disappears from Windows display settings
- `Install firewall rules`: asks for administrator permission and opens inbound TCP `17660` plus UDP `17659`, `17670`, and `17691` on private/domain networks
- `Open log`: opens `led_host_tray.log`
- `Exit`: stops the host if it was started by the tray app, removes the virtual display device, then exits the tray app

Keep `led_host_tray.exe` and `led_host_app.exe` in the same directory.
The driver scripts are resolved from the source tree, so the current development layout expects the tray app under `build\windows-host\Release`.

## LAN Discovery

Linux clients advertise themselves with UDP `LED_CLIENT_V1` beacons on port `17659`.
Windows sends `LED_DISCOVER_V1` probes and can send `LED_CONNECT_V1` to ask a selected Linux client to connect back to the Windows host.
If discovered clients do not appear, run `Install firewall rules` from the Windows tray menu once.

The video/control path is unchanged: Windows still listens on TCP `17660`, sends RTP video on UDP `17670`, and receives input on UDP `17691`.

The virtual display is session-scoped: it should not be present before `Start extended display`.
If no Linux client connects within 10 seconds, or if the host exits unexpectedly, the tray app removes the virtual display automatically.

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
