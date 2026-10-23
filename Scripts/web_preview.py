# Предпросмотр веб-интерфейса на ПК без платы: python Scripts/web_preview.py [порт]  ->  http://localhost:8080
# Отдаёт страницы из CO2_Sensor_NRF24/web/ (как в прошивке - через build_web.render_page, файлы читаются
# заново при каждом запросе - правка страницы видна сразу по F5) и имитирует API прошивки:
#   GET  /api/vars    - тестовые значения (MOCK ниже), состояние меняется записью;
#   POST /api/set     - разбор как в прошивке (urlencoded UTF-8), принятые пары печатаются в консоль;
#   GET  /history.csv - синтетическая история за сутки;
#   GET  /errlog.txt  - пример журнала ошибок (ERRLOG_TXT);
#   GET  /i2ctest.txt - пример отчёта теста I2C (I2C_TEST_TXT).
# Неизвестное прошивке имя в /api/set возвращается как "unknown" - так же, как отвечает устройство.
import json
import math
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_web import WEB_DIR, MIME, render_page  # noqa: E402

FAN_PARAM = "cfg_fan_"
BOOT = int(time.time()) - 3 * 3600
FANS = [{"name": "Кухня", "fl": 0, "fspt": 0, "spc": 2, "tst": 0},
        {"name": "Спальня", "fl": 2, "fspt": 1800, "spc": 4, "tst": 0},
        {"name": "Ванная", "fl": 1, "fspt": 0, "spc": 0, "tst": 3}]
FAN_CFG = [{"cfg_fan_name": f["name"], "cfg_fan_rf_ch": 2, "cfg_fan_addr_LSB": "0x%02X" % (0xC1 + i),
            "cfg_fan_min": 0, "cfg_fan_max": 6, "cfg_fan_override_day": 0, "cfg_fan_override_night": 3,
            "cfg_fan_day": 0, "cfg_fan_night": 2, "cfg_fan_flags": f["fl"], "cfg_fan_pause": 10, "cfg_fan_timeout": 60} for i, f in enumerate(FANS)]
MOCK = {
    "CO2_current": 812, "sntp_status": "NTP", "sys_mactime": BOOT,
    "sys_ver": "2.0.0", "sys_build": "05.10.2026 12:00 (предпросмотр)",
    "sys_author": "Вадим (vad7@yahoo.com)", "sys_board": "ПК", "sys_fw_board": "es3c28p", "sys_fw_id": "es3c28p_k22", "sys_sensor": "S8 LP", "sys_heap": "180000 / 7900000 байт",
    "now_night": 0, "night": 0, "now_night_ov": 0, "fans_off": 0, "fan_speed_previous": 2, "fsp_time_def": 180,
    "history_count": 1440, "cfg_co2_fans": len(FANS), "cfg_co2_fans_speed_th": "500,600,700,800,900,1000",
    "cfg_co2_fans_speed_delta": 50, "cfg_co2_period": 20, "cfg_co2_night_start": "22:00",
    "cfg_co2_night_end": "06:00", "cfg_co2_night_start_wd": "23:00", "cfg_co2_night_end_wd": "08:00",
    "cfg_co2_night_max": 2, "cfg_co2_refresh_t": 5, "cfg_co2_bright_day": 80,
    "cfg_co2_bright_night": 10, "cfg_co2_radio_mode": 0, "cfg_co2_passive_ch": 120, "cfg_co2_radio_reset": 3600, "radio_rx_count": 0, "temp_en": 1, "temp_ds_ok": 1, "cfg_temp_sensor": 2, "temp_c": "23.4", "temp_rh": 45, "temp_status": "23.4 °C, 45 % (SHT40, I2C 0x44, ошибок 0, повторов 1)", "temp_serial": "28-0316A2794AFF", "temp_res": 11, "cfg_temp_period": 10, "cfg_co2_poll": 2, "cfg_i2c_khz": 100, "i2c_khz_max": 100, "cfg_sens_retries": 3, "cfg_sens_retry_ms": 100, "cfg_co2_off_delta": 30, "cfg_hist_days": 7, "cfg_digits_font": 1, "hist_cap": 30240, "hist_wanted": 30240, "hist_max": 1664000, "hist_bytes": 151200, "hist_psram": 1, "hist_rec_bytes": 5, "cpu_load0": 12, "cpu_load1": 35, "chip_temp": "45.3", "rtos_stack_warn": 512, "rtos_overflow": 0, "rtos_tasks": [["loopTask", 0, 1, 1, 5232, 412345, 60], ["IDLE0", 1, 0, 0, 724, 1650000, 470], ["IDLE1", 0, 0, 1, 732, 1500000, 420], ["httpd", 2, 5, -1, 4310, 2345, 3], ["wifi", 2, 23, 0, 3604, 30123, 25], ["tiT", 2, 18, -1, 380, 12001, 12], ["esp_timer", 2, 22, 0, 2980, 1500, 4]], "net_ntp_period": 180, "cfg_vars_fans_speed_ov": 0,
    "net_ssid": "Дом-WiFi", "net_ap_ssid": "CO2-Hub", "net_ap_delay": 10, "net_tz": "MSK-3", "net_ntp": "pool.ntp.org", "net_ip": "127.0.0.1", "net_rssi": -61,
    "abc_state": 0, "abc_period": 192, "abc_on": 1, "abc_msg": "",
    "net_mode": "подключено к Wi-Fi", "co2_status": "OK, 812 ppm, статус 0x00, ошибок обмена: 0, повторов: 4",
    "radio_regs": "CONFIG=7E EN_AA=01 SETUP_RETR=2F (предпросмотр)",
}
# Имена, которые принимает setVar() прошивки, но не возвращает writeVars() (команды)
COMMANDS = {FAN_PARAM, "co2_abc_write", "cfg_co2_save", "cfg_co2_save_fans", "cfg_vars_save", "net_save", "net_pass", "net_forget",
            "cfg_fan_override", "sys_touch_recal", "sys_time_set"}


def fan_index(v):
    try:
        i = int(v)
    except (TypeError, ValueError):
        return 0
    return i if 0 <= i < len(FANS) else 0


def var_groups():
    """Группы /api/vars из web.cpp (VAR_GROUPS и функции varsXxx) - как в прошивке и check_web.js."""
    import re
    cpp = open(os.path.join(os.path.dirname(WEB_DIR), "src", "web.cpp"), encoding="utf-8").read()
    tbl = re.search(r"static const VarGroup VAR_GROUPS\[\] = \{([\s\S]*?)\n\};", cpp).group(1)
    res = {}
    for g, fn in re.findall(r'\{\s*"(\w+)",\s*(\w+)\s*\}', tbl):
        i = cpp.index("static void %s(" % fn)
        res[g] = set(re.findall(r'j\.\w+\s*\(\s*"(\w+)"', cpp[i:cpp.index("\n}\n", i)]))
    return res


VAR_GROUPS = var_groups()


def vars_json(fan):
    now = int(time.time())
    v = dict(MOCK)
    v.update({"CO2_last_time": now - 7, "sntp_time": now, "sys_uptime": now - BOOT, "cfg_fan_idx": fan})
    v.update(FAN_CFG[fan])
    v["fans"] = [dict(f, ttm=now - 15) for f in FANS]
    return v


ERRLOG_TXT = (
    "20.10 09:15:02 - 10:23:31 x210  CO2 S8: нет ответа\r\n"
    "20.10 08:02:40  DS18B20: нет ответа датчика\r\n"
    "20.10 07:55:10 - 08:01:12 x4  SHT40 (0x44): нет ответа по I2C\r\n"
    "20.10 07:41:03  Тач FT6336G: касание есть, чтение по I2C не удалось\r\n"
    "19.10 23:58:03 - 20.10 07:12:45 x120  Вентилятор 3 (Ванная): нет подтверждения (ACK)\r\n"
    "+0:00:01  nRF24: CONFIG = 0x00\r\n"
    "+0:00:01  nRF24: модуль не найден (нет ответа по SPI)\r\n")


# Отчёт теста I2C (i2c_test.cpp buildReport), сокращён
I2C_TEST_TXT = (
    "Тест I2C: 2026-10-23 12:00:00, шина 100 кГц, плата ES3C28P, датчик CO2 K22-OC\n"
    "Время - от начала до конца транзакции I2C (esp_timer); ожидания кратны проходу loop() (~1 мс).\n"
    "\nПроход 1: опрос размазан по циклу 1000 мс (K22 - 0, SHT40 - 1/3, тач - 2/3 цикла), датчики - по 50 сеансов\n"
    "  K22 (CO2): сеансов 50, удачных 50\n"
    "    мс                      мин      ср.     макс\n"
    "    запрос                 0.62     0.64     0.71\n"
    "    ожидание              18.02    21.40    35.11\n"
    "    ответ                  0.58     0.60     0.66\n"
    "    итого                 19.30    22.65    36.40\n"
    "    чтений ответа за сеанс: мин 9, ср. 11.2, макс 17\n"
    "    ошибок: запрос 0, нет ответа за 160 мс 0, неверный ответ 0\n"
    "\nПроход 3: K22, первое чтение через паузу, до 3 чтений через 20 мс, по 50 сеансов\n"
    "    пауза, мс     1-е    2-е    3-е    нет  запрос  неверн.  сеанс ср.\n"
    "    100            50      0      0      0       0        0     101.25\n")


def history_csv():
    now = int(time.time())
    d = ";"                                   # как прошивка: колонки «;», дробная часть «,»
    lines = ["date%svalue%stemp" % (d, d)]
    for k in range(1440):
        t = now - k * 60
        ppm = int(650 + 250 * math.sin(t / 7200.0) + 60 * math.sin(t / 900.0))
        if 300 <= k < 330: ppm = ""                # пропуск: датчик CO2 молчал (записи «нет данных»)
        temp = "" if k >= 1439 else ("%.1f" % (22.0 + 1.5 * math.sin(t / 5400.0))).replace(".", ",")   # самая старая - до первого показания t
        lines.append(time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + d + str(ppm) + d + temp)
    return "\r\n".join(lines) + "\r\n"


class Handler(BaseHTTPRequestHandler):
    def send(self, code, ctype, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def api_set(self, query):
        res = {"ok": True, "restart": False}
        fan = 0
        print("/api/set:")
        for k, v in parse_qsl(query, keep_blank_values=True, encoding="utf-8", errors="replace"):
            print("   %s = %r" % (k, v))
            if "�" in v:
                print("   ВНИМАНИЕ: значение не в UTF-8 (клиент кодирует не так, как браузер)")
            if k == FAN_PARAM:
                fan = fan_index(v)
            elif k in FAN_CFG[fan]:
                FAN_CFG[fan][k] = v
                if k == "cfg_fan_name":
                    FANS[fan]["name"] = v
            elif k in MOCK:
                MOCK[k] = v
            elif k == "sys_touch_recal":
                res["restart"] = v == "1"
            elif k == "co2_abc_read":          # ABC: в прошивке - асинхронно; здесь сразу «готово»
                MOCK["abc_state"], MOCK["abc_msg"] = 2, ""
            elif k == "co2_abc_period":
                MOCK["abc_period"] = int(v)
            elif k == "co2_abc_on":
                MOCK["abc_on"] = int(v)
            elif k not in COMMANDS and "unknown" not in res:
                res["unknown"] = k
        self.send(200, "application/json; charset=utf-8", json.dumps(res, ensure_ascii=False))

    def do_GET(self):
        u = urlsplit(self.path)
        p = u.path.lstrip("/") or "index.htm"
        if p == "api/vars":
            q = dict(parse_qsl(u.query))
            v = vars_json(fan_index(q.get(FAN_PARAM)))
            if q.get("g"):   # как прошивка: только запрошенные группы + sys
                keep = set(VAR_GROUPS["sys"])
                for g in q["g"].split(","):
                    keep |= VAR_GROUPS.get(g, set())
                v = {k: x for k, x in v.items() if k in keep}
            return self.send(200, "application/json; charset=utf-8", json.dumps(v, ensure_ascii=False))
        if p == "api/set":
            return self.api_set(u.query)
        if p == "history.csv":
            return self.send(200, "text/csv; charset=utf-8", history_csv())
        if p == "i2ctest.txt":
            return self.send(200, "text/plain; charset=utf-8", I2C_TEST_TXT)
        if p == "errlog.txt":
            return self.send(200, "text/plain; charset=utf-8", ERRLOG_TXT)
        ext = os.path.splitext(p)[1]
        full = os.path.join(WEB_DIR, p)
        if ext not in MIME or "/" in p or not os.path.isfile(full):
            return self.send(404, "text/plain; charset=utf-8", "Not found")
        if ext == ".htm":
            return self.send(200, MIME[ext], render_page(WEB_DIR, p))
        with open(full, "rb") as f:
            return self.send(200, MIME[ext], f.read())

    def do_POST(self):
        if urlsplit(self.path).path != "/api/set":
            return self.send(405, "text/plain; charset=utf-8", "POST: /api/set")
        n = int(self.headers.get("Content-Length") or 0)
        return self.api_set(self.rfile.read(n).decode("ascii", errors="replace"))

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("Предпросмотр: http://localhost:%d  (Ctrl+C - выход)" % port)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
