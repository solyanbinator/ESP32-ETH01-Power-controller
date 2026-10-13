#!/usr/bin/env python3
"""Агент выключения сервера для ESP32 ETH01 Power controller (Proxmox VE или любой Linux с systemd).

ESP32 обращается к нему по HTTP:
  GET /ping       200, пока система работает (ESP32 проверяет доступность по ICMP и этому пути)
  GET /poweroff   200 и штатное выключение хоста: systemctl poweroff, Proxmox сам останавливает
                  виртуальные машины и контейнеры по их настройкам (порядок, таймауты)

Защита. ESP32 не передаёт заголовки, поэтому ключ передаётся в пути команды:
  --allow 192.168.19.100   принимать запросы только с этих адресов или подсетей (через запятую)
  --token / POWER_SERVER_TOKEN
                           /poweroff только с ?key=<ключ>; в веб-интерфейсе ESP32 путь команды
                           выключения: /poweroff?key=<ключ>. Ключ — латиница, цифры, - . _ ~

Wake-on-LAN включается в BIOS/UEFI и на сетевой карте. --wol-interface eno1 при запуске и перед
выключением выполняет «ethtool -s eno1 wol g» (физическая карта, не мост vmbr0).

Установка на Proxmox (от root):
  mkdir -p /opt/power-server
  cp power_server.py /opt/power-server/
  cp power-server.service /etc/systemd/system/
  nano /etc/default/power-server            # параметры, пример — в power-server.service
  systemctl daemon-reload
  systemctl enable --now power-server
  journalctl -u power-server -f             # журнал
Проверка без выключения: добавьте --dry-run в POWER_SERVER_ARGS и перезапустите службу.
Если включён firewall Proxmox — разрешите входящий TCP-порт (по умолчанию 8080) с адреса ESP32.

Только стандартная библиотека Python 3.7+.
"""
import argparse
import hmac
import ipaddress
import json
import os
import shlex
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def log(message):
    print(message, flush=True)  # время добавляет journald


def run(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as e:
        log(f"{' '.join(command)}: {e}")
        return False
    if result.returncode != 0:
        log(f"{' '.join(command)}: код {result.returncode} {result.stderr.strip()}")
        return False
    return True


class Agent:
    def __init__(self, args):
        self.args = args
        self.allow = [ipaddress.ip_network(item.strip(), strict=False)
                      for item in args.allow.split(",") if item.strip()] if args.allow else []
        self.lock = threading.Lock()
        self.shutting_down = False

    def allowed(self, address):
        if not self.allow:
            return True
        ip = ipaddress.ip_address(address)
        return any(ip in network for network in self.allow)

    def token_ok(self, key):
        return not self.args.token or hmac.compare_digest(key.encode(), self.args.token.encode())

    def arm_wol(self):
        if self.args.wol_interface and run(["ethtool", "-s", self.args.wol_interface, "wol", "g"]):
            log(f"Wake-on-LAN включён на {self.args.wol_interface}")

    def poweroff(self):
        """Повторная команда во время выключения не запускает его заново."""
        with self.lock:
            if self.shutting_down:
                return 200, {"status": "poweroff_in_progress"}
            self.shutting_down = True
        threading.Thread(target=self._shutdown, daemon=True).start()
        return 200, {"status": "poweroff_simulated" if self.args.dry_run else "poweroff_started"}

    def _shutdown(self):
        time.sleep(1)  # дать ответу дойти до ESP32
        self.arm_wol()
        command = shlex.split(self.args.command)
        if self.args.dry_run:
            log(f"dry-run: выключение не выполняется ({' '.join(command)})")
            with self.lock:
                self.shutting_down = False
            return
        log(f"Выключение: {' '.join(command)}")
        if not run(command):
            with self.lock:
                self.shutting_down = False


def make_handler(agent):
    class Handler(BaseHTTPRequestHandler):
        server_version = "power-server"

        def do_GET(self):
            self.handle_request()

        def do_POST(self):
            self.handle_request()

        def handle_request(self):
            client = self.client_address[0]
            url = urllib.parse.urlsplit(self.path)
            if not agent.allowed(client):
                log(f"Отклонено: {client} {url.path} (нет в --allow)")
                return self.reply(403, {"detail": "Forbidden"})
            if url.path == agent.args.ping_path:
                return self.reply(200, {"status": "ok"})
            if url.path == agent.args.poweroff_path:
                key = dict(urllib.parse.parse_qsl(url.query)).get("key") or self.headers.get("X-API-Key", "")
                if not agent.token_ok(key):
                    log(f"Отклонено: {client} {url.path} (неверный ключ)")
                    return self.reply(403, {"detail": "Forbidden"})
                log(f"Команда выключения от {client}")
                return self.reply(*agent.poweroff())
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

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Агент выключения сервера для ESP32 ETH01 Power controller")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--ping-path", default="/ping")
    parser.add_argument("--poweroff-path", default="/poweroff")
    parser.add_argument("--allow", help="разрешённые адреса/подсети через запятую, например 192.168.19.100")
    parser.add_argument("--token", default=os.environ.get("POWER_SERVER_TOKEN"),
                        help="ключ для /poweroff?key=..., по умолчанию из POWER_SERVER_TOKEN")
    parser.add_argument("--wol-interface", help="сетевая карта для ethtool ... wol g, например eno1")
    parser.add_argument("--command", default="systemctl poweroff", help="команда выключения")
    parser.add_argument("--dry-run", action="store_true", help="не выключать, только писать в журнал")
    args = parser.parse_args()

    agent = Agent(args)
    log(f"power-server: {args.host}:{args.port}, ping {args.ping_path}, poweroff {args.poweroff_path}, "
        f"allow {args.allow or 'все'}, ключ {'задан' if args.token else 'не задан'}, "
        f"команда '{args.command}'{', DRY-RUN' if args.dry_run else ''}")
    if not args.allow and not args.token:
        log("ВНИМАНИЕ: без --allow и ключа выключить сервер может любой в сети")
    agent.arm_wol()
    server = ThreadingHTTPServer((args.host, args.port), make_handler(agent))
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
