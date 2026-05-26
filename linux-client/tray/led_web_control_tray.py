#!/usr/bin/env python3
import os
import socket
import subprocess
import sys

CLIENT_SERVICE = "lan-extended-display-client.service"
WEB_SERVICE = "lan-extended-display-web-desktop.service"


def run_command(command):
    try:
        return subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=8,
            check=False,
        ).stdout.strip()
    except Exception as exc:
        return str(exc)


def service_state(service):
    result = subprocess.run(
        ["systemctl", "--user", "is-active", service],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.strip() or "unknown"


def local_ips():
    ips = []
    try:
        output = subprocess.check_output(
            ["ip", "-o", "-4", "addr", "show", "scope", "global"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        for line in output.splitlines():
            parts = line.split()
            if "inet" in parts:
                address = parts[parts.index("inet") + 1].split("/")[0]
                if not address.startswith("127."):
                    ips.append(address)
    except Exception:
        pass
    if not ips:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.connect(("8.8.8.8", 80))
                ips.append(sock.getsockname()[0])
        except Exception:
            ips.append("0.0.0.0")
    return ips


def launch_terminal(title, command):
    shell_command = f"{command}; printf '\\nPress Enter to close...'; read _"
    terminal = os.environ.get("TERMINAL", "")
    candidates = [
        [terminal, "-e", "sh", "-lc", shell_command] if terminal else None,
        ["x-terminal-emulator", "-e", "sh", "-lc", shell_command],
        ["xterm", "-T", title, "-e", "sh", "-lc", shell_command],
    ]
    for candidate in candidates:
        if not candidate:
            continue
        try:
            subprocess.Popen(candidate)
            return
        except Exception:
            continue


class WebControlTray:
    def __init__(self):
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("GdkPixbuf", "2.0")
        from gi.repository import GdkPixbuf
        from gi.repository import GLib, Gtk

        self.glib = GLib
        self.gtk = Gtk
        self.gdk_pixbuf = GdkPixbuf
        if not self.gtk.init_check()[0]:
            raise RuntimeError("GTK init failed")
        self.icon = self.gtk.StatusIcon.new_from_pixbuf(self.create_monitor_icon())
        self.icon.set_visible(True)
        self.icon.connect("popup-menu", self.on_popup_menu)
        self.glib.timeout_add_seconds(3, self.refresh_tooltip)
        self.refresh_tooltip()

    def create_monitor_icon(self):
        rows = []
        for y in range(32):
            row = []
            for x in range(32):
                pixel = "T"
                if 1 <= x <= 30 and 1 <= y <= 30:
                    pixel = "."
                if 5 <= x <= 26 and 7 <= y <= 21:
                    pixel = "S"
                if ((5 <= x <= 26 and y in (7, 21)) or
                        (7 <= y <= 21 and x in (5, 26))):
                    pixel = "B"
                if 8 <= x <= 23 and y == 10:
                    pixel = "G"
                if 8 <= x <= 20 and y == 13:
                    pixel = "G"
                if 14 <= x <= 18 and 22 <= y <= 25:
                    pixel = "B"
                if 10 <= x <= 22 and 26 <= y <= 28:
                    pixel = "B"
                row.append(pixel)
            rows.append("".join(row))
        xpm = [
            "32 32 5 1",
            "T c None",
            ". c #12181C",
            "S c #2BAEBE",
            "B c #EEF4F6",
            "G c #7AE2EC",
        ] + rows
        return self.gdk_pixbuf.Pixbuf.new_from_xpm_data(xpm)

    def tooltip_text(self):
        client = service_state(CLIENT_SERVICE)
        web = service_state(WEB_SERVICE)
        ips = ", ".join(local_ips())
        return f"LAN Extended Display Control\nClient: {client}\nWeb desktop: {web}\nIP: {ips}"

    def refresh_tooltip(self):
        self.icon.set_tooltip_text(self.tooltip_text())
        return True

    def service_action(self, action, service):
        output = run_command(["systemctl", "--user", action, service])
        if output:
            print(output, flush=True)
        self.refresh_tooltip()

    def add_item(self, menu, label, callback, sensitive=True):
        item = self.gtk.MenuItem(label=label)
        item.set_sensitive(sensitive)
        item.connect("activate", callback)
        menu.append(item)

    def on_popup_menu(self, _icon, button, activate_time):
        menu = self.gtk.Menu()
        client_state = service_state(CLIENT_SERVICE)
        web_state = service_state(WEB_SERVICE)
        ips = ", ".join(local_ips())

        status = self.gtk.MenuItem(label=f"Client: {client_state}   Web: {web_state}")
        status.set_sensitive(False)
        menu.append(status)
        ip_item = self.gtk.MenuItem(label=f"IP: {ips}")
        ip_item.set_sensitive(False)
        menu.append(ip_item)
        menu.append(self.gtk.SeparatorMenuItem())

        self.add_item(menu, "Start extended-display client", lambda *_: self.service_action("start", CLIENT_SERVICE))
        self.add_item(menu, "Stop extended-display client", lambda *_: self.service_action("stop", CLIENT_SERVICE))
        self.add_item(menu, "Restart extended-display client", lambda *_: self.service_action("restart", CLIENT_SERVICE))
        menu.append(self.gtk.SeparatorMenuItem())

        self.add_item(
            menu,
            "Open client log",
            lambda *_: launch_terminal(
                "LAN Extended Display client log",
                f"journalctl --user -u {CLIENT_SERVICE} -f",
            ),
        )
        self.add_item(
            menu,
            "Open web desktop log",
            lambda *_: launch_terminal(
                "LAN Extended Display web log",
                f"journalctl --user -u {WEB_SERVICE} -f",
            ),
        )
        self.add_item(menu, "Open terminal", lambda *_: launch_terminal("Terminal", "exec sh"))
        menu.append(self.gtk.SeparatorMenuItem())
        self.add_item(menu, "Refresh status", lambda *_: self.refresh_tooltip())
        self.add_item(menu, "Quit this icon", lambda *_: self.gtk.main_quit())

        menu.show_all()
        menu.popup(None, None, None, None, button, activate_time)

    def run(self):
        self.gtk.main()


def main():
    try:
        WebControlTray().run()
    except Exception as exc:
        print(f"web control tray failed: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
