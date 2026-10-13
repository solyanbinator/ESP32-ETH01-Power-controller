"""Эмулятор управляемого сервера для проверки ESP32 ETH01 Power controller на Windows.
На реальном сервере (Proxmox) работает power_server.py.

Ведёт себя примерно как реальный компьютер:
  * включён        — отвечает на HTTP GET /ping и /poweroff;
  * /poweroff      — начинается выключение: shutdown_s секунд сервер ещё отвечает,
                     потом HTTP-порт закрывается;
  * выключен       — HTTP-порт закрыт, как у выключенной машины; ждёт Wake-on-LAN по UDP;
  * magic packet   — со своим MAC будит выключенный сервер: boot_s секунд загрузки,
                     потом HTTP-порт снова открыт. Во время работы и выключения пакет
                     игнорируется, как реальной сетевой картой.

Неисправности для проверки повторов команд:
  --poweroff-mode normal | ignore (отвечает 200, но не выключается) | error (403, как без API-ключа)
  --wol-mode      normal | ignore (magic packet не будит)

Управление для автотестов (только с этого компьютера), http://127.0.0.1:<control-port>:
  GET /state                    состояние, режимы и журнал событий
  GET|POST /control?power=on|off&poweroff_mode=..&wol_mode=..&shutdown_s=..&boot_s=..&clear=1

ICMP эмулятор не контролирует: на ping отвечает (или нет) сам компьютер. Windows в
«Общественной» сети по умолчанию не отвечает; тестовая прошивка умеет отключать ICMP-проверку.

Только стандартная библиотека Python 3.8+:
  py test_server.py                    включён, принимает WOL для любого MAC
  py test_server.py --mac AA:BB:CC:DD:EE:FF --state off --boot-s 40
"""
import argparse
import json
import socket
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

POWEROFF_MODES = ("normal", "ignore", "error")
WOL_MODES = ("normal", "ignore")


def log(message):
    print(time.strftime("%H:%M:%S"), message, flush=True)


def normalize_mac(mac):
    hex_digits = mac.replace(":", "").replace("-", "").upper()
    if len(hex_digits) != 12 or any(c not in "0123456789ABCDEF" for c in hex_digits):
        raise argparse.ArgumentTypeError("MAC в формате AA:BB:CC:DD:EE:FF")
    return ":".join(hex_digits[i:i + 2] for i in range(0, 12, 2))


def parse_magic_packet(data):
    """MAC из magic packet (6 x 0xFF и 16 повторов MAC) или None."""
    if len(data) < 102 or data[:6] != b"\xff" * 6:
        return None
    mac = data[6:12]
    if data[6:102] != mac * 16:
        return None
    return ":".join(f"{b:02X}" for b in mac)


class Device:
    def __init__(self, args):
        self.args = args
        self.lock = threading.RLock()
        self.mac = args.mac
        self.poweroff_mode = args.poweroff_mode
        self.wol_mode = args.wol_mode
        self.shutdown_s = args.shutdown_s
        self.boot_s = args.boot_s
        self.state = "off"
        self.pings = 0
        self.events = []
        self.http = None
        self.generation = 0  # смена режима отменяет запланированные переходы
        if args.state == "on":
            self._power_on()
        else:
            log("state off")

    # --- журнал ---

    def event(self, kind, **data):
        with self.lock:
            self.events.append({"t": round(time.time(), 3), "kind": kind, **data})
        if kind == "state":
            log(f"=== {data['state']} ===")
        else:
            log(kind + " " + " ".join(f"{k}={v}" for k, v in data.items()))

    def snapshot(self):
        with self.lock:
            return {
                "state": self.state,
                "http_listening": self.http is not None,
                "mac": self.mac,
                "poweroff_mode": self.poweroff_mode,
                "wol_mode": self.wol_mode,
                "shutdown_s": self.shutdown_s,
                "boot_s": self.boot_s,
                "pings": self.pings,
                "events": list(self.events),
            }

    # --- HTTP-порт сервера: открыт, пока «ОС» работает ---

    def _open_http(self):
        if self.http:
            return
        server = ThreadingHTTPServer((self.args.host, self.args.port), make_device_handler(self))
        server.daemon_threads = True
        self.http = server
        threading.Thread(target=server.serve_forever, daemon=True).start()

    def _close_http(self):
        if not self.http:
            return
        server, self.http = self.http, None
        server.shutdown()
        server.server_close()

    def _set_state(self, state):
        self.state = state
        self.event("state", state=state)

    def _power_on(self):
        self._open_http()
        self._set_state("on")

    def _power_off(self):
        self._close_http()
        self._set_state("off")

    def _schedule(self, delay, action):
        self.generation += 1
        generation = self.generation

        def run():
            with self.lock:
                if generation == self.generation:
                    action()

        timer = threading.Timer(delay, run)
        timer.daemon = True
        timer.start()

    # --- реакция на ESP32 ---

    def ping(self):
        with self.lock:
            self.pings += 1

    def poweroff(self, client):
        with self.lock:
            self.event("poweroff_request", src=client, mode=self.poweroff_mode, state=self.state)
            if self.poweroff_mode == "error":
                return 403, {"detail": "Forbidden"}
            if self.poweroff_mode == "normal" and self.state == "on":
                self._set_state("shutting_down")
                self._schedule(self.shutdown_s, self._power_off)
            return 200, {"status": "poweroff_started"}

    def magic_packet(self, mac, client):
        with self.lock:
            match = self.mac is None or mac == self.mac
            self.event("wol_packet", src=client, mac=mac, match=match, state=self.state)
            if match and self.wol_mode == "normal" and self.state == "off":
                self._set_state("booting")
                self._schedule(self.boot_s, self._power_on)

    # --- управление из автотестов ---

    def control(self, params):
        with self.lock:
            if "poweroff_mode" in params:
                if params["poweroff_mode"] not in POWEROFF_MODES:
                    raise ValueError(f"poweroff_mode: {', '.join(POWEROFF_MODES)}")
                self.poweroff_mode = params["poweroff_mode"]
            if "wol_mode" in params:
                if params["wol_mode"] not in WOL_MODES:
                    raise ValueError(f"wol_mode: {', '.join(WOL_MODES)}")
                self.wol_mode = params["wol_mode"]
            if "shutdown_s" in params:
                self.shutdown_s = float(params["shutdown_s"])
            if "boot_s" in params:
                self.boot_s = float(params["boot_s"])
            if "mac" in params:
                self.mac = normalize_mac(params["mac"]) if params["mac"] else None
            if params.get("clear"):
                self.events.clear()
                self.pings = 0
            if "power" in params:
                if params["power"] not in ("on", "off"):
                    raise ValueError("power: on, off")
                self.generation += 1  # отменить идущую загрузку или выключение
                if params["power"] == "on":
                    self._power_on()
                else:
                    self._power_off()
            return self.snapshot()


def make_device_handler(device):
    class DeviceHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path == device.args.ping_path:
                device.ping()
                self.reply(200, {"status": "ok"})
            elif path == device.args.poweroff_path:
                self.reply(*device.poweroff(self.client_address[0]))
            else:
                self.reply(404, {"detail": "Not Found"})

        def reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    return DeviceHandler


def make_control_handler(device):
    class ControlHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path == "/state":
                self.reply(200, device.snapshot())
            elif url.path == "/control":
                self.control(dict(urllib.parse.parse_qsl(url.query)))
            else:
                self.reply(404, {"error": "not found"})

        def do_POST(self):
            url = urllib.parse.urlsplit(self.path)
            length = int(self.headers.get("Content-Length") or 0)
            params = dict(urllib.parse.parse_qsl(url.query))
            params.update(urllib.parse.parse_qsl(self.rfile.read(length).decode()))
            if url.path == "/control":
                self.control(params)
            else:
                self.reply(404, {"error": "not found"})

        def control(self, params):
            try:
                self.reply(200, device.control(params))
            except (ValueError, argparse.ArgumentTypeError) as e:
                self.reply(400, {"error": str(e)})

        def reply(self, code, body):
            data = json.dumps(body, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    return ControlHandler


def wol_listener(device, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", port))
    while True:
        data, addr = sock.recvfrom(2048)
        mac = parse_magic_packet(data)
        if mac:
            device.magic_packet(mac, addr[0])


def main():
    parser = argparse.ArgumentParser(description="Эмулятор сервера для ESP32 ETH01 Power controller")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080, help="HTTP-порт сервера")
    parser.add_argument("--ping-path", default="/ping")
    parser.add_argument("--poweroff-path", default="/poweroff")
    parser.add_argument("--wol-port", type=int, default=9)
    parser.add_argument("--mac", type=normalize_mac, help="MAC сервера; без него WOL принимается для любого MAC")
    parser.add_argument("--state", choices=("on", "off"), default="on", help="состояние при запуске")
    parser.add_argument("--shutdown-s", type=float, default=15, help="длительность выключения")
    parser.add_argument("--boot-s", type=float, default=30, help="длительность загрузки")
    parser.add_argument("--poweroff-mode", choices=POWEROFF_MODES, default="normal")
    parser.add_argument("--wol-mode", choices=WOL_MODES, default="normal")
    parser.add_argument("--control-port", type=int, default=8081, help="порт управления (127.0.0.1)")
    args = parser.parse_args()

    log(f"emulator: http {args.host}:{args.port} (ping {args.ping_path}, poweroff {args.poweroff_path}), "
        f"wol udp {args.wol_port} mac {args.mac or 'any'}, shutdown {args.shutdown_s}s, boot {args.boot_s}s")
    device = Device(args)
    threading.Thread(target=wol_listener, args=(device, args.wol_port), daemon=True).start()
    control = ThreadingHTTPServer(("127.0.0.1", args.control_port), make_control_handler(device))
    log(f"control: http://127.0.0.1:{args.control_port}/state")
    try:
        control.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
