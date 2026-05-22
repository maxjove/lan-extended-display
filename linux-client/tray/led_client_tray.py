#!/usr/bin/env python3
import argparse
import os
import signal
import socket
import subprocess
import sys
import threading
import time

DISCOVERY_PORT = 17659
CONTROL_PORT = 17660
INPUT_PORT = 17691


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


def parse_fields(message):
    fields = {}
    for part in message.strip().split(";"):
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    return fields


class ClientController:
    def __init__(self, client_bin, sink, drain_ms, input_mode, video_mode):
        self.client_bin = os.path.expanduser(client_bin)
        self.sink = sink
        self.drain_ms = str(drain_ms)
        self.input_mode = input_mode
        self.video_mode = video_mode
        self.process = None
        self.lock = threading.Lock()
        self.last_host = ""

    def is_running(self):
        with self.lock:
            return self.process is not None and self.process.poll() is None

    def status(self):
        return "running" if self.is_running() else "ready"

    def start(self, host, control_port=CONTROL_PORT, input_port=INPUT_PORT):
        host = host.strip()
        if not host:
            return
        with self.lock:
            replaced = self.process is not None and self.process.poll() is None
            if self.process is not None and self.process.poll() is None:
                print(f"replacing active client session for host {self.last_host or 'unknown'}", flush=True)
            self.stop_locked()
            if replaced:
                time.sleep(0.25)
            if self.video_mode == "mjpeg":
                command = [
                    self.client_bin,
                    "--receive-mjpeg-stream",
                    host,
                    str(control_port),
                    "0",
                    self.drain_ms,
                    self.input_mode,
                    str(input_port),
                ]
            else:
                command = [
                    self.client_bin,
                    "--receive-test-stream",
                    host,
                    str(control_port),
                    "0",
                    self.sink,
                    self.drain_ms,
                    self.input_mode,
                    str(input_port),
                ]
            env = os.environ.copy()
            env.setdefault("DISPLAY", ":0")
            env.setdefault("XAUTHORITY", os.path.expanduser("~/.Xauthority"))
            print(f"starting client for host {host} control={control_port} input={input_port}", flush=True)
            self.process = subprocess.Popen(command, env=env, start_new_session=True)
            self.last_host = host

    def stop_locked(self):
        if self.process is None:
            return
        if self.process.poll() is None:
            print(f"stopping client pid={self.process.pid}", flush=True)
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            except Exception:
                self.process.terminate()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                print(f"client pid={self.process.pid} did not exit after SIGTERM; killing", flush=True)
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                except Exception:
                    self.process.kill()
                self.process.wait(timeout=2)
        self.process = None

    def stop(self):
        with self.lock:
            self.stop_locked()


class DiscoveryService:
    def __init__(self, controller):
        self.controller = controller
        self.stop_event = threading.Event()
        self.name = socket.gethostname()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=2)

    def beacon(self):
        ips = ",".join(local_ips())
        return f"LED_CLIENT_V1;name={self.name};ip={ips};status={self.controller.status()};control={CONTROL_PORT};input={INPUT_PORT};"

    def run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.bind(("", DISCOVERY_PORT))
            sock.settimeout(1.0)
            last_beacon = 0.0
            while not self.stop_event.is_set():
                now = time.monotonic()
                if now - last_beacon >= 3.0:
                    data = self.beacon().encode("utf-8")
                    try:
                        sock.sendto(data, ("255.255.255.255", DISCOVERY_PORT))
                    except OSError as exc:
                        print(f"discovery beacon failed: {exc}", flush=True)
                    last_beacon = now
                try:
                    raw, address = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    break
                message = raw.decode("utf-8", errors="ignore")
                if message.startswith("LED_DISCOVER_V1"):
                    try:
                        sock.sendto(self.beacon().encode("utf-8"), (address[0], DISCOVERY_PORT))
                    except OSError as exc:
                        print(f"discovery reply failed to {address[0]}: {exc}", flush=True)
                elif message.startswith("LED_CONNECT_V1"):
                    fields = parse_fields(message)
                    host = fields.get("host", address[0])
                    control = int(fields.get("control", CONTROL_PORT))
                    input_port = int(fields.get("input", INPUT_PORT))
                    print(f"connect command from {address[0]} host={host} control={control} input={input_port}", flush=True)
                    self.controller.start(host, control, input_port)


class TrayUi:
    def __init__(self, controller, discovery):
        self.controller = controller
        self.discovery = discovery
        self.gtk = None
        self.status_icon = None
        self.timer_id = None

    def run(self):
        if not os.environ.get("DISPLAY"):
            self.run_console()
            return
        try:
            import gi

            gi.require_version("Gtk", "3.0")
            from gi.repository import GLib, Gtk

            self.gtk = Gtk
            self.glib = GLib
        except Exception:
            self.run_console()
            return
        if not self.gtk.init_check()[0]:
            self.run_console()
            return

        self.status_icon = self.gtk.StatusIcon.new_from_icon_name("network-workgroup")
        self.status_icon.set_visible(True)
        self.status_icon.connect("popup-menu", self.on_popup_menu)
        self.timer_id = self.glib.timeout_add_seconds(2, self.refresh_tooltip)
        self.refresh_tooltip()
        self.gtk.main()

    def tooltip_text(self):
        ip_text = ", ".join(local_ips())
        host = self.controller.last_host or "none"
        return f"LAN Extended Display\nStatus: {self.controller.status()}\nIP: {ip_text}\nHost: {host}"

    def refresh_tooltip(self):
        if self.status_icon is not None:
            self.status_icon.set_tooltip_text(self.tooltip_text())
        return True

    def on_popup_menu(self, icon, button, activate_time):
        menu = self.gtk.Menu()
        item_status = self.gtk.MenuItem(label=f"Status: {self.controller.status()}")
        item_status.set_sensitive(False)
        menu.append(item_status)
        item_ip = self.gtk.MenuItem(label=f"IP: {', '.join(local_ips())}")
        item_ip.set_sensitive(False)
        menu.append(item_ip)
        menu.append(self.gtk.SeparatorMenuItem())
        item_stop = self.gtk.MenuItem(label="Stop client")
        item_stop.connect("activate", lambda *_: self.controller.stop())
        menu.append(item_stop)
        item_quit = self.gtk.MenuItem(label="Quit tray")
        item_quit.connect("activate", self.quit)
        menu.append(item_quit)
        menu.show_all()
        menu.popup(None, None, None, None, button, activate_time)

    def quit(self, *_):
        self.discovery.stop()
        self.controller.stop()
        self.gtk.main_quit()

    def run_console(self):
        print(self.tooltip_text(), flush=True)
        while True:
            time.sleep(5)
            print(self.tooltip_text(), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--client-bin", default=os.path.expanduser("~/lan-extended-display/led_client_app"))
    parser.add_argument("--default-host", default="")
    parser.add_argument("--sink", default="avdec-i420gl")
    parser.add_argument("--drain-ms", default=1000, type=int)
    parser.add_argument("--input-mode", default="x11-input")
    parser.add_argument("--video-mode", choices=("mjpeg", "h264"), default="mjpeg")
    args = parser.parse_args()

    controller = ClientController(args.client_bin, args.sink, args.drain_ms, args.input_mode, args.video_mode)
    discovery = DiscoveryService(controller)

    def handle_signal(_signum, _frame):
        discovery.stop()
        controller.stop()
        sys.exit(0)

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    discovery.start()
    if args.default_host:
        controller.start(args.default_host)
    TrayUi(controller, discovery).run()


if __name__ == "__main__":
    main()
