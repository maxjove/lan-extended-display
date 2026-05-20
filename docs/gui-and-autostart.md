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

- `Start extended display`: asks for administrator permission, installs the IddCx virtual display, then starts `led_host_app.exe --serve-live-capture 17660 17670 17691 0 60 sendinput 20000 1920 1080`
- `Stop extended display`: asks the running host to stop through `Local\LanExtendedDisplayHostStop`, then removes the IddCx root display device so it disappears from Windows display settings
- `Exit`: stops the host if it was started by the tray app, removes the virtual display device, then exits the tray app

Keep `led_host_tray.exe` and `led_host_app.exe` in the same directory.
The driver scripts are resolved from the source tree, so the current development layout expects the tray app under `build\windows-host\Release`.

The virtual display is session-scoped: it should not be present before `Start extended display`.
If no Linux client connects within 10 seconds, or if the host exits unexpectedly, the tray app removes the virtual display automatically.

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
