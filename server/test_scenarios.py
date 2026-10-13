"""Автотесты сценариев ESP32 ETH01 Power controller: настоящая плата + эмулятор сервера (test_server.py).

Сценарии автоматики требуют тестовую прошивку — в ней входы IO35/IO36 и ICMP-проверка
подменяются через POST /api/test (на рабочей прошивке выполняются только тесты без подмены):
    idf.py -B build_test -DAPP_TEST_HOOKS=ON -p COM7 flash
Запуск (эмулятор запускается сам на портах 8080/9, управление 127.0.0.1:8081):
    py test_scenarios.py --esp 192.168.19.50
    py test_scenarios.py --esp 192.168.19.50 --only power_loss manual_restart

На время тестов в ESP записываются короткие задержки и MAC эмулятора; после тестов исходные
настройки восстанавливаются, подмена входов снимается. Только стандартная библиотека Python 3.8+.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_MAC = "02:00:00:E5:32:01"  # локально администрируемый: реальную машину не разбудит
TEST_TIMINGS = {
    "mains_lost_s": 5,
    "mains_restore_s": 5,
    "ping_interval_s": 2,
    "off_wait_s": 20,
    "on_wait_s": 20,
    "attempts": 2,
}
SHUTDOWN_S = 6
BOOT_S = 8
WOL_PACKETS_PER_COMMAND = 3  # WOL_REPEAT в прошивке
CONFIG_KEYS = ["ip", "port", "mac", "wol_port", "ping_path", "poweroff_path", "mains_lost_s",
               "mains_restore_s", "ping_interval_s", "off_wait_s", "on_wait_s", "attempts",
               "outputs_enabled", "io32_off_s", "io32_on_s", "io33_off_s", "io33_on_s"]


class Failure(Exception):
    pass


def expect(condition, message):
    if not condition:
        raise Failure(message)


# ESP и эмулятор в локальной сети: системный прокси (http_proxy) их не видит, ходим напрямую
DIRECT = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http(method, url, fields=None, timeout=5):
    data = urllib.parse.urlencode(fields).encode() if fields is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    try:
        with DIRECT.open(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return e.code, json.loads(body)
        except ValueError:
            return e.code, {"raw": body.decode(errors="replace")}


class Esp:
    def __init__(self, ip):
        self.base = f"http://{ip}"

    def status(self):
        return http("GET", self.base + "/status")[1]

    def post_config(self, **fields):
        return http("POST", self.base + "/api/config", fields)

    def config(self, **fields):
        for key, value in fields.items():
            code, body = self.post_config(**{key: value})
            if code != 200:
                raise Failure(f"настройка {key}={value}: HTTP {code} {body}")

    def inputs(self, **fields):
        code, body = http("POST", self.base + "/api/test", fields)
        if code != 200:
            raise Failure(f"/api/test {fields}: HTTP {code} {body}")


class Emulator:
    def __init__(self, control_port):
        self.base = f"http://127.0.0.1:{control_port}"

    def control(self, **params):
        code, body = http("POST", self.base + "/control", {k: str(v) for k, v in params.items()})
        if code != 200:
            raise Failure(f"эмулятор {params}: {body}")
        return body

    def state(self):
        return http("GET", self.base + "/state")[1]

    def events(self, kind, **match):
        return [e for e in self.state()["events"]
                if e["kind"] == kind and all(e.get(k) == v for k, v in match.items())]


def describe(st):
    a = st["auto"]
    return (f"сервер {st['server']['state']}, питание {a['mains']}, сценарий {a['scenario']} "
            f"({a['attempts']}/{a['max_attempts']}), последний {a['last']['scenario']}/{a['last']['result']}")


class Context:
    """Опрос /status с журналом переходов сценариев для проверки последовательностей."""

    def __init__(self, esp, emu, hooks):
        self.esp = esp
        self.emu = emu
        self.hooks = hooks  # тестовая прошивка: входы подменяются через /api/test
        self.seen = []
        self._scenario = None
        self._last = None
        self._waiting = False

    def poll(self):
        st = self.esp.status()
        a = st["auto"]
        if a["scenario"] != self._scenario:
            self._scenario = a["scenario"]
            if a["scenario"] != "none":
                self.seen.append(("start", a["scenario"]))
        last = (a["last"]["scenario"], a["last"]["result"], a["last"]["attempts"], a["last"]["ago_s"])
        if self._last and last[:3] != self._last[:3] or (self._last and last[3] < self._last[3]):
            self.seen.append(("finish",) + last[:3])
        self._last = last
        if a["waiting_shutdown"] and not self._waiting:
            self.seen.append(("waiting_shutdown",))
        self._waiting = a["waiting_shutdown"]
        return st

    def wait(self, predicate, timeout, what):
        deadline = time.time() + timeout
        while True:
            st = self.poll()
            if predicate(st):
                return st
            if time.time() > deadline:
                raise Failure(f"за {timeout} с не дождались: {what}; сейчас: {describe(st)}")
            time.sleep(0.4)

    def hold(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.poll()
            time.sleep(0.4)

    def finished(self, scenario):
        return [e for e in self.seen if e[0] == "finish" and e[1] == scenario]


def settle(ctx, server_on, **emu):
    """Исходное состояние: сервер вкл/выкл, питание подтверждено, сценариев нет."""
    ctx.emu.control(power="on" if server_on else "off", poweroff_mode="normal", wol_mode="normal",
                    shutdown_s=SHUTDOWN_S, boot_s=BOOT_S)
    mains, state = ("present", "online") if server_on else ("lost", "offline")
    if ctx.hooks:
        ctx.esp.inputs(power_failure="0" if server_on else "1", button="0")
        ctx.wait(lambda s: s["auto"]["mains"] == mains and s["auto"]["scenario"] == "none"
                 and s["auto"]["confirm_left_s"] == 0 and s["server"]["state"] == state,
                 90, f"исходное состояние: питание {mains}, сервер {state}, сценариев нет")
    else:
        ctx.wait(lambda s: s["server"]["state"] == state, 30, f"исходное состояние: сервер {state}")
    ctx.emu.control(clear=1, **emu)
    ctx._last = None
    ctx.poll()
    ctx.seen.clear()


def expect_last(st, scenario, result, attempts=None):
    last = st["auto"]["last"]
    got = f"{last['scenario']}/{last['result']}, команд {last['attempts']}"
    expect(last["scenario"] == scenario and last["result"] == result, f"ожидался {scenario}/{result}, получен {got}")
    if attempts is not None:
        expect(last["attempts"] == attempts, f"ожидалось команд {attempts}, получен {got}")


def scenario_is(name):
    return lambda s: s["auto"]["scenario"] == name


def idle(s):
    return s["auto"]["scenario"] == "none"


# ------------------------------------------------------------------ тесты

def t_validation(ctx):
    before = ctx.esp.status()["config"]
    bad = {"ip": "192.168.1", "port": "70000", "mac": "01:00:5E:00:00:01", "ping_path": "/a b",
           "poweroff_path": "", "mains_lost_s": "0", "attempts": "11", "unknown_key": "1"}
    for key, value in bad.items():
        code, body = ctx.esp.post_config(**{key: value})
        expect(code == 400 and body.get("error"), f"{key}={value!r} принят: HTTP {code} {body}")
    expect(ctx.esp.status()["config"] == before, "после отклонённых запросов настройки изменились")
    code, body = ctx.esp.post_config(mac="02-00-00-e5-32-01")
    expect(code == 200 and body["config"]["mac"] == TEST_MAC, f"MAC не нормализован: {body}")
    return f"{len(bad)} некорректных значений отклонены, MAC нормализован"


def t_detection(ctx):
    settle(ctx, True)
    ctx.emu.control(power="off")
    t = time.time()
    st = ctx.wait(lambda s: s["server"]["state"] == "offline", 15, "сервер офлайн")
    off_s = time.time() - t
    expect(not st["server"]["http"]["ok"], f"HTTP-проверка: {st['server']['http']}")
    ctx.emu.control(power="on")
    t = time.time()
    st = ctx.wait(lambda s: s["server"]["state"] == "online", 15, "сервер онлайн")
    expect(st["server"]["http"]["detail"] == "HTTP 200", f"HTTP-проверка: {st['server']['http']}")
    if ctx.hooks:
        expect(not ctx.seen, f"сценарии не должны запускаться от состояния сервера: {ctx.seen}")
    return f"офлайн замечен через {off_s:.1f} с, онлайн через {time.time() - t:.1f} с"


def t_responsive(ctx):
    # Раньше веб зависал: /status сам ждал ping сервера. Сервер выключен, ICMP включён —
    # проверки длятся секунды, а ответ /status должен оставаться мгновенным.
    settle(ctx, True)
    ctx.emu.control(power="off")
    if ctx.hooks:
        ctx.esp.inputs(icmp="auto")
    latencies = []
    try:
        for _ in range(30):
            t = time.time()
            ctx.esp.status()
            latencies.append(time.time() - t)
            time.sleep(0.3)
    finally:
        if ctx.hooks:
            ctx.esp.inputs(icmp="off")
        ctx.emu.control(power="on")
    worst = max(latencies)
    expect(worst < 1.0, f"/status отвечал до {worst * 1000:.0f} мс")
    return f"30 запросов /status: среднее {sum(latencies) / len(latencies) * 1000:.0f} мс, максимум {worst * 1000:.0f} мс"


def t_flicker(ctx):
    settle(ctx, True)
    ctx.esp.inputs(power_failure="1")
    ctx.hold(2)
    ctx.esp.inputs(power_failure="0")
    ctx.hold(7)
    expect(not ctx.seen, f"запустился сценарий: {ctx.seen}")
    expect(not ctx.emu.events("poweroff_request"), "серверу ушёл Power OFF")
    return "пропадание на 2 с при подтверждении 5 с проигнорировано"


def t_power_loss(ctx):
    settle(ctx, True)
    ctx.esp.inputs(power_failure="1")
    ctx.wait(lambda s: s["auto"]["confirm_left_s"] > 0, 3, "обратный отсчёт подтверждения")
    t = time.time()
    ctx.wait(scenario_is("power_off"), 10, "запуск сценария выключения")
    confirm_s = time.time() - t
    st = ctx.wait(idle, 45, "завершение сценария")
    expect_last(st, "power_off", "done", 1)
    expect(len(ctx.emu.events("poweroff_request")) == 1, "ожидался один Power OFF")
    expect(ctx.emu.state()["state"] == "off", "эмулятор не выключился")
    return f"подтверждение {confirm_s:.1f} с, 1 команда Power OFF, сервер выключен"


def t_power_restore(ctx):
    settle(ctx, False)
    ctx.esp.inputs(power_failure="0")
    ctx.wait(scenario_is("power_on"), 10, "запуск сценария включения")
    st = ctx.wait(idle, 45, "завершение сценария")
    expect_last(st, "power_on", "done", 1)
    packets = ctx.emu.events("wol_packet", match=True)
    expect(len(packets) == WOL_PACKETS_PER_COMMAND, f"magic packet: {len(packets)}, ожидалось {WOL_PACKETS_PER_COMMAND}")
    expect(ctx.emu.state()["state"] == "on", "эмулятор не включился")
    return f"1 команда WOL ({len(packets)} magic packet), сервер загрузился"


def t_already_off(ctx):
    settle(ctx, True)
    ctx.emu.control(power="off")
    ctx.wait(lambda s: s["server"]["state"] == "offline", 15, "сервер офлайн")
    ctx.esp.inputs(power_failure="1")
    ctx.wait(lambda s: ctx.finished("power_off"), 15, "завершение сценария выключения")
    st = ctx.poll()
    expect_last(st, "power_off", "done", 0)
    expect(not ctx.emu.events("poweroff_request"), "Power OFF отправлен уже выключенному серверу")
    return "сервер уже выключен: сценарий завершён без команд"


def t_poweroff_ignored(ctx):
    settle(ctx, True, poweroff_mode="ignore")
    ctx.esp.inputs(power_failure="1")
    ctx.wait(scenario_is("power_off"), 10, "запуск сценария выключения")
    st = ctx.wait(idle, 75, "завершение сценария")
    expect_last(st, "power_off", "failed", 2)
    requests = ctx.emu.events("poweroff_request")
    expect(len(requests) == 2, f"Power OFF: {len(requests)}, ожидалось 2")
    gap = requests[1]["t"] - requests[0]["t"]
    expect(abs(gap - TEST_TIMINGS["off_wait_s"]) < 3, f"интервал между повторами {gap:.1f} с")
    return f"2 команды с интервалом {gap:.1f} с, сценарий «не выполнен»"


def t_poweroff_forbidden(ctx):
    settle(ctx, True, poweroff_mode="error")
    ctx.esp.inputs(power_failure="1")
    ctx.wait(lambda s: ctx.emu.events("poweroff_request"), 15, "Power OFF")
    st = ctx.wait(lambda s: s["command"]["name"] == "poweroff" and not s["command"]["busy"]
                  and s["command"]["detail"] == "HTTP 403", 5, "ответ HTTP 403 в результате команды")
    expect(not st["command"]["ok"], f"команда с ответом 403 считается выполненной: {st['command']}")
    st = ctx.wait(idle, 75, "завершение сценария")
    expect_last(st, "power_off", "failed", 2)
    return "ответ 403 показан как ошибка команды, 2 попытки, сценарий «не выполнен»"


def t_wol_ignored(ctx):
    settle(ctx, False, wol_mode="ignore")
    ctx.esp.inputs(power_failure="0")
    ctx.wait(scenario_is("power_on"), 10, "запуск сценария включения")
    st = ctx.wait(idle, 75, "завершение сценария")
    expect_last(st, "power_on", "failed", 2)
    packets = ctx.emu.events("wol_packet")
    expect(len(packets) == 2 * WOL_PACKETS_PER_COMMAND, f"magic packet: {len(packets)}")
    return f"2 команды WOL ({len(packets)} пакетов), сервер не проснулся, сценарий «не выполнен»"


def t_slow_boot(ctx):
    settle(ctx, False, boot_s=28)
    ctx.esp.inputs(power_failure="0")
    ctx.wait(scenario_is("power_on"), 10, "запуск сценария включения")
    st = ctx.wait(idle, 80, "завершение сценария")
    expect_last(st, "power_on", "done", 2)
    expect(ctx.emu.state()["state"] == "on", "эмулятор не включился")
    return "загрузка 28 с дольше ожидания 20 с: второй WOL, затем сервер онлайн"


def t_restore_during_shutdown(ctx):
    # Питание вернулось, пока сервер ещё выключается после Power OFF. Раньше шаг «включение»
    # видел онлайн и сразу завершался, а сервер через несколько секунд выключался насовсем.
    settle(ctx, True, shutdown_s=18)
    ctx.esp.inputs(power_failure="1")
    ctx.wait(lambda s: ctx.emu.events("poweroff_request"), 15, "Power OFF")
    ctx.esp.inputs(power_failure="0")
    ctx.wait(scenario_is("power_on"), 10, "запуск сценария включения")
    expect(ctx.finished("power_off") and ctx.finished("power_off")[-1][2] == "aborted",
           f"сценарий выключения не прерван: {ctx.seen}")
    ctx.wait(lambda s: ("waiting_shutdown",) in ctx.seen, 5, "ожидание завершения выключения")
    st = ctx.wait(idle, 70, "завершение сценария включения")
    expect_last(st, "power_on", "done")
    expect(ctx.emu.events("wol_packet", match=True), "WOL не отправлен после выключения")
    expect(ctx.emu.state()["state"] == "on", f"сервер остался в состоянии {ctx.emu.state()['state']}")
    return "выключение прервано, дождались офлайна, WOL — сервер снова включён"


def t_manual_restart(ctx):
    settle(ctx, True)
    ctx.esp.inputs(button="1")
    ctx.wait(lambda s: s["auto"]["hold_left_s"] > 0, 2, "обратный отсчёт удержания")
    t = time.time()
    ctx.wait(scenario_is("manual_off"), 7, "запуск ручного перезапуска")
    hold_s = time.time() - t
    ctx.esp.inputs(button="0")
    st = ctx.wait(lambda s: idle(s) and ctx.finished("manual_on"), 70, "завершение перезапуска")
    expect_last(st, "manual_on", "done", 1)
    off = ctx.finished("manual_off")
    expect(off and off[-1][2] == "done", f"шаг выключения: {off}")
    expect(len(ctx.emu.events("poweroff_request")) == 1, "ожидался один Power OFF")
    expect(ctx.emu.events("wol_packet", match=True), "WOL не отправлен")
    expect(ctx.emu.state()["state"] == "on", "сервер не включился")
    return f"удержание {hold_s:.1f} с, Power OFF -> офлайн -> WOL -> онлайн"


def t_manual_short(ctx):
    settle(ctx, True)
    ctx.esp.inputs(button="1")
    ctx.hold(2)
    ctx.esp.inputs(button="0")
    ctx.hold(4)
    expect(not ctx.seen, f"запустился сценарий: {ctx.seen}")
    return "нажатие на 2 с проигнорировано"


def t_manual_poweroff_ignored(ctx):
    settle(ctx, True, poweroff_mode="ignore")
    ctx.esp.inputs(button="1")
    ctx.wait(scenario_is("manual_off"), 7, "запуск ручного перезапуска")
    ctx.esp.inputs(button="0")
    st = ctx.wait(lambda s: idle(s) and ctx.finished("manual_on"), 110, "завершение перезапуска")
    off = ctx.finished("manual_off")
    expect(off and off[-1][2:] == ("timeout", 2), f"шаг выключения: {off}")
    expect(("waiting_shutdown",) in ctx.seen, "шаг включения не ждал возможного выключения")
    expect_last(st, "manual_on", "done", 0)
    expect(not ctx.emu.events("wol_packet"), "WOL отправлен работающему серверу")
    return "сервер не выключился: 2 Power OFF, переход к включению, сервер остался онлайн"


def outputs(st):
    o = st["outputs"]
    return o["io32"]["high"], o["io33"]["high"]


def no_mains_scenarios(ctx):
    """Выходы проверяются отдельно от сценариев сервера: подтверждение питания — 10 часов."""
    ctx.esp.config(mains_lost_s=36000, mains_restore_s=36000)


def restore_mains_timings(ctx):
    ctx.esp.inputs(power_failure="0")
    ctx.esp.config(outputs_enabled=0, mains_lost_s=TEST_TIMINGS["mains_lost_s"],
                   mains_restore_s=TEST_TIMINGS["mains_restore_s"])


def t_outputs_disabled(ctx):
    settle(ctx, True)
    no_mains_scenarios(ctx)
    try:
        ctx.esp.config(outputs_enabled=0, io32_off_s=1, io33_off_s=1)
        ctx.esp.inputs(power_failure="1")
        ctx.hold(3)
        st = ctx.poll()
        expect(not st["outputs"]["enabled"] and outputs(st) == (True, True), f"выходы: {st['outputs']}")
    finally:
        restore_mains_timings(ctx)
    return "управление выключено: питания нет 3 с при задержке 1 с — оба выхода HIGH"


def t_outputs_follow_power(ctx):
    settle(ctx, True)
    no_mains_scenarios(ctx)
    delays = {"io32_off_s": 3, "io32_on_s": 6, "io33_off_s": 8, "io33_on_s": 2}
    try:
        ctx.esp.config(outputs_enabled=1, **delays)
        ctx.wait(lambda s: outputs(s) == (True, True), 5, "оба выхода HIGH при наличии питания")

        ctx.esp.inputs(power_failure="1")
        ctx.hold(1.5)
        ctx.esp.inputs(power_failure="0")
        ctx.hold(1)
        expect(outputs(ctx.poll()) == (True, True), "пропадание на 1,5 с переключило выход")

        t0 = time.time()
        ctx.esp.inputs(power_failure="1")
        ctx.wait(lambda s: s["outputs"]["io32"]["change_in_s"] > 0, 2, "обратный отсчёт IO32")
        ctx.wait(lambda s: not outputs(s)[0], 6, "IO32 LOW")
        off32 = time.time() - t0
        expect(outputs(ctx.poll())[1], "IO33 выключился раньше своей задержки")
        ctx.wait(lambda s: not outputs(s)[1], 9, "IO33 LOW")
        off33 = time.time() - t0

        t1 = time.time()
        ctx.esp.inputs(power_failure="0")
        ctx.wait(lambda s: outputs(s)[1], 5, "IO33 HIGH")
        on33 = time.time() - t1
        expect(not outputs(ctx.poll())[0], "IO32 включился раньше своей задержки")
        ctx.wait(lambda s: outputs(s)[0], 8, "IO32 HIGH")
        on32 = time.time() - t1
    finally:
        restore_mains_timings(ctx)
    measured = {"io32_off_s": off32, "io32_on_s": on32, "io33_off_s": off33, "io33_on_s": on33}
    for key, value in measured.items():
        expect(abs(value - delays[key]) < 1.2, f"{key}: {value:.1f} с вместо {delays[key]} с")
    return (f"IO32: LOW через {off32:.1f} с (задано 3), HIGH через {on32:.1f} с (6); "
            f"IO33: LOW через {off33:.1f} с (8), HIGH через {on33:.1f} с (2); пропадание на 1,5 с проигнорировано")


# (имя, описание, функция, нужна тестовая прошивка)
TESTS = [
    ("validation", "Валидация настроек", t_validation, False),
    ("detection", "Определение онлайн/офлайн", t_detection, False),
    ("responsive", "Веб не зависает при недоступном сервере", t_responsive, False),
    ("flicker", "Кратковременное пропадание питания", t_flicker, True),
    ("power_loss", "Пропадание питания: выключение", t_power_loss, True),
    ("power_restore", "Появление питания: включение", t_power_restore, True),
    ("already_off", "Пропадание питания при выключенном сервере", t_already_off, True),
    ("poweroff_ignored", "Сервер игнорирует Power OFF: повторы", t_poweroff_ignored, True),
    ("poweroff_forbidden", "Power OFF отклонён (403)", t_poweroff_forbidden, True),
    ("wol_ignored", "Сервер не просыпается от WOL: повторы", t_wol_ignored, True),
    ("slow_boot", "Загрузка дольше ожидания", t_slow_boot, True),
    ("restore_during_shutdown", "Питание вернулось во время выключения", t_restore_during_shutdown, True),
    ("manual_restart", "Кнопка: перезапуск", t_manual_restart, True),
    ("manual_short", "Кнопка: короткое нажатие", t_manual_short, True),
    ("manual_poweroff_ignored", "Кнопка: сервер не выключается", t_manual_poweroff_ignored, True),
    ("outputs_disabled", "Выходы IO32/IO33: управление выключено", t_outputs_disabled, True),
    ("outputs_follow_power", "Выходы IO32/IO33: задержки по питанию", t_outputs_follow_power, True),
]


def local_ip_for(remote_ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((remote_ip, 80))
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    parser = argparse.ArgumentParser(description="Автотесты сценариев ESP32 ETH01 Power controller")
    parser.add_argument("--esp", required=True, help="IP-адрес ESP32")
    parser.add_argument("--host-ip", help="IP этого компьютера для ESP (по умолчанию определяется сам)")
    parser.add_argument("--emu-port", type=int, default=8080)
    parser.add_argument("--wol-port", type=int, default=9)
    parser.add_argument("--control-port", type=int, default=8081)
    parser.add_argument("--no-emulator", action="store_true", help="эмулятор уже запущен вручную")
    parser.add_argument("--only", nargs="*", help="запустить только перечисленные тесты")
    args = parser.parse_args()

    esp = Esp(args.esp)
    try:
        st = esp.status()
    except OSError as e:
        sys.exit(f"ESP {args.esp} не отвечает: {e}")
    hooks = bool(st.get("test_hooks"))
    if not hooks:
        print("На ESP рабочая прошивка: сценарии автоматики будут пропущены. Для них прошейте тестовую:\n"
              "  idf.py -B build_test -DAPP_TEST_HOOKS=ON -p COM7 flash\n")
    original = {key: st["config"][key] for key in CONFIG_KEYS}
    host_ip = args.host_ip or local_ip_for(args.esp)

    emu_proc = None
    emu_log = None
    if not args.no_emulator:
        emu_log = tempfile.NamedTemporaryFile("w", suffix="_emulator.log", delete=False, encoding="utf-8")
        emu_proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "test_server.py"), "--port", str(args.emu_port),
             "--wol-port", str(args.wol_port), "--control-port", str(args.control_port),
             "--mac", TEST_MAC, "--state", "on", "--shutdown-s", str(SHUTDOWN_S), "--boot-s", str(BOOT_S)],
            stdout=emu_log, stderr=subprocess.STDOUT)
    emu = Emulator(args.control_port)
    for _ in range(50):
        try:
            emu.state()
            break
        except OSError:
            time.sleep(0.1)

    ctx = Context(esp, emu, hooks)
    results = []
    selected = [t for t in TESTS if not args.only or t[0] in args.only]
    print(f"ESP {args.esp}, эмулятор {host_ip}:{args.emu_port}, WOL udp {args.wol_port}, MAC {TEST_MAC}")
    print("Тестовые задержки: " + ", ".join(f"{k}={v}" for k, v in TEST_TIMINGS.items()), flush=True)
    try:
        if hooks:
            esp.inputs(power_failure="0", button="0", icmp="off")
        esp.config(ip=host_ip, port=args.emu_port, mac=TEST_MAC, wol_port=args.wol_port,
                   ping_path="/ping", poweroff_path="/poweroff", **TEST_TIMINGS)
        ctx.wait(lambda s: s["server"]["state"] == "online", 25,
                 "ESP видит эмулятор (если нет — разрешите python.exe в брандмауэре Windows)")
        for name, title, func, needs_hooks in selected:
            if needs_hooks and not hooks:
                results.append((name, title, None, 0, "пропущен: нужна тестовая прошивка"))
                continue
            print(f"\n[{name}] {title} ...", flush=True)
            t = time.time()
            try:
                details = func(ctx)
                results.append((name, title, True, time.time() - t, details))
                print(f"  PASS ({time.time() - t:.0f} с): {details}", flush=True)
            except Failure as e:
                results.append((name, title, False, time.time() - t, str(e)))
                print(f"  FAIL ({time.time() - t:.0f} с): {e}", flush=True)
                print(f"  переходы: {ctx.seen}", flush=True)
    finally:
        print("\nВосстановление настроек ESP ...", flush=True)
        try:
            esp.config(**original)
            if hooks:
                esp.inputs(power_failure="auto", button="auto", icmp="auto")
        except (Failure, OSError) as e:
            print(f"  не удалось восстановить: {e}; исходные настройки: {original}")
        if emu_proc:
            emu_proc.terminate()
            emu_proc.wait(timeout=5)
            emu_log.close()
            print(f"Журнал эмулятора: {emu_log.name}")

    passed = sum(1 for r in results if r[2])
    failed = sum(1 for r in results if r[2] is False)
    skipped = sum(1 for r in results if r[2] is None)
    print(f"\nИтог: пройдено {passed}, ошибок {failed}, пропущено {skipped}")
    for name, title, ok, duration, details in results:
        mark = "SKIP" if ok is None else "PASS" if ok else "FAIL"
        print(f"  {mark}  {title} ({duration:.0f} с) — {details}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
