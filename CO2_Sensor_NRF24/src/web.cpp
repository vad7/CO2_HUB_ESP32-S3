// web.cpp — HTTP-сервер (esp_http_server, своя задача). Перенос web_int_vars.c / web_int_callbacks.c хаба.
//   GET  /<файл>          — статические страницы и скрипты (встроены в прошивку, gzip; без подстановок);
//   GET  /api/vars        — переменные одним JSON-объектом, чанками по группам: ?g=main,fans — только эти
//                           группы (+ sys всегда), без g — все; ?cfg_fan_=N — выбранный вентилятор;
//   POST /api/set         — запись переменных, тело application/x-www-form-urlencoded (UTF-8, %XX);
//   GET  /api/set?a=b&... — то же из строки запроса (для ручной проверки и скриптов);
//   GET  /history.csv     — история CO2;
//   POST /api/ota         — обновление прошивки: тело — firmware.bin (application/octet-stream), затем перезапуск.
// Пароль настроек (net.webPass, HTTP Basic, имя — любое; пустой — не запрашивается): страницы настроек
// (AUTH_PAGES), /api/ota и /api/set — кроме оперативного управления с главной (OPEN_VARS: поправка скорости,
// ночь, коррекция вентилятора). Браузер спрашивает пароль один раз и дальше шлёт его сам.
// Строки (имена вентиляторов, SSID, пароль) приходят в UTF-8 с процентным кодированием (браузер —
// URLSearchParams / encodeURIComponent), уходят в JSON с экранированием. Обрезка по размеру поля —
// по границе символа UTF-8 (кириллица — 2 байта на букву).
// Доступ к настройкам и состоянию — под Cfg::lock() (loop() работает параллельно).
#include "web.h"
#include <Arduino.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "config.h"
#include "build_info.h"
#include "hub_config.h"
#include "fan_control.h"
#include "history.h"
#include "co2_sensor.h"
#include "net.h"
#include "utf8.h"
#include "temp_sensor.h"
#include "sys_info.h"
#include "web_files.h"

namespace Web {

constexpr size_t   VAR_NAME_MAX    = 48;
constexpr size_t   PATH_MAX_LEN    = 48;
constexpr size_t   LINE_MAX_LEN    = 192;
constexpr size_t   REPORT_LEN      = 200;
constexpr size_t   JSON_RESP_LEN   = 160;
constexpr uint32_t S_PER_MIN       = 60;
constexpr int16_t  TENTHS_PER_DEG  = 10;     // температура в 0.1 °C
constexpr uint32_t US_PER_MS       = 1000;   // время задач FreeRTOS: мкс -> мс
constexpr const char* INDEX_FILE   = "index.htm";
constexpr const char* HISTORY_CSV  = "history.csv";
constexpr const char* API_VARS     = "api/vars";
constexpr const char* API_SET      = "api/set";
constexpr const char* API_OTA      = "api/ota";
// Пароль настроек: страницы и переменные /api/set без пароля
static const char* const AUTH_PAGES[] = { "settings.htm", "setfans.htm" };
static const char* const OPEN_VARS[]  = { "cfg_fan_", "cfg_fan_override", "cfg_vars_fans_speed_ov", "now_night_ov" };
constexpr const char* AUTH_HDR     = "Authorization";
constexpr const char* AUTH_BASIC   = "Basic ";
constexpr size_t      AUTH_HDR_MAX = 128;              // «Basic » + base64(имя:пароль), пароль до 32 байт
constexpr size_t      AUTH_DEC_MAX = 96;
constexpr const char* FAN_PARAM    = "cfg_fan_";       // выбранный вентилятор (Web_cfg_fan_ хаба)
constexpr const char* GROUPS_PARAM = "g";              // /api/vars?g=main,fans — группы переменных
constexpr const char* GROUPS_SEP   = ",";
constexpr size_t      VARS_GROUPS_ARG_MAX = 96;        // строка групп (все 10 имён — ~45 байт)
constexpr const char* MIME_JSON    = "application/json; charset=utf-8";
constexpr const char* CACHE_STATIC = "max-age=86400";
constexpr const char* CACHE_NONE   = "no-cache";

static httpd_handle_t s_server = nullptr;
static char s_body[WEB_POST_MAX + 1];      // тело POST / строка запроса (одна задача сервера)
static char s_json[WEB_JSON_MAX];          // группа ответа /api/vars (собирается под замком, отправляется без него)
static char s_radioReport[REPORT_LEN] = "";

// Контекст запроса: выбранный вентилятор и что сделать после разбора переменных
struct ReqCtx {
    uint8_t fan;
    bool saveCo2, saveFans, saveVars, saveNet, recalcAll, reconnect, historyResize, restart;
    char unknown[VAR_NAME_MAX];            // первое неизвестное имя (ответ /api/set)
};

// ------------------------------------------------------------------ вывод чанками (history.csv)
class Out {
public:
    explicit Out(httpd_req_t* r) : m_req(r) {}
    void write(const char* d, size_t len)
    {
        while (len) {
            size_t k = sizeof(m_buf) - m_n;
            if (k > len) k = len;
            memcpy(m_buf + m_n, d, k);
            m_n += k; d += k; len -= k;
            if (m_n == sizeof(m_buf)) flush();
        }
    }
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char t[LINE_MAX_LEN];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(t, sizeof(t), fmt, ap);
        va_end(ap);
        if (n > 0) write(t, (size_t)n < sizeof(t) ? (size_t)n : sizeof(t) - 1);
    }
    void flush()
    {
        if (m_n && m_ok) m_ok = httpd_resp_send_chunk(m_req, m_buf, m_n) == ESP_OK;
        m_n = 0;
    }
    void end() { flush(); httpd_resp_send_chunk(m_req, nullptr, 0); }
private:
    httpd_req_t* m_req;
    char   m_buf[WEB_OUT_BUF];
    size_t m_n  = 0;
    bool   m_ok = true;
};

// ------------------------------------------------------------------ JSON в фиксированный буфер
class Json {
public:
    Json(char* buf, size_t size) : m_buf(buf), m_size(size) { m_buf[0] = '\0'; }
    void beginObj(const char* k = nullptr) { sep(k); raw("{"); m_comma = false; }
    void endObj()                          { raw("}"); m_comma = true; }
    void beginArr(const char* k)           { sep(k); raw("["); m_comma = false; }
    void endArr()                          { raw("]"); m_comma = true; }
    void num(const char* k, long v)          { sep(k); rawf("%ld", v); }
    void unum(const char* k, unsigned long v) { sep(k); rawf("%lu", v); }
    void u64(const char* k, uint64_t v)      { sep(k); rawf("%llu", (unsigned long long)v); }
    void boolean(const char* k, bool v)      { sep(k); raw(v ? "true" : "false"); }
    void str(const char* k, const char* v)   { sep(k); quoted(v); }
    void strf(const char* k, const char* fmt, ...) __attribute__((format(printf, 3, 4)))
    {
        char t[LINE_MAX_LEN];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t, sizeof(t), fmt, ap);
        va_end(ap);
        str(k, t);
    }
    bool   overflow() const { return m_over; }
    size_t length()   const { return m_len; }
private:
    void raw(const char* s) { put(s, strlen(s)); }
    void rawf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char t[LINE_MAX_LEN];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(t, sizeof(t), fmt, ap);
        va_end(ap);
        if (n > 0) put(t, (size_t)n < sizeof(t) ? (size_t)n : sizeof(t) - 1);
    }
    void put(const char* s, size_t n)
    {
        if (m_over || m_len + n >= m_size) { m_over = true; return; }
        memcpy(m_buf + m_len, s, n);
        m_len += n;
        m_buf[m_len] = '\0';
    }
    void sep(const char* k)
    {
        if (m_comma) raw(",");
        m_comma = true;
        if (k) { quoted(k); raw(":"); }
    }
    // Экранирование JSON: '"', '\', управляющие символы; байты UTF-8 — как есть
    void quoted(const char* s)
    {
        raw("\"");
        for (; *s; s++) {
            const uint8_t ch = (uint8_t)*s;
            if (ch == '"' || ch == '\\') { const char e[2] = { '\\', (char)ch }; put(e, 2); }
            else if (ch < 0x20)          rawf("\\u%04x", ch);
            else                         put(s, 1);
        }
        raw("\"");
    }
    char*  m_buf;
    size_t m_size;
    size_t m_len   = 0;
    bool   m_comma = false;
    bool   m_over  = false;
};

// ------------------------------------------------------------------ утилиты
static const WebFile* findFile(const char* path)
{
    for (size_t i = 0; i < WEB_FILES_COUNT; i++)
        if (strcmp(WEB_FILES[i].path, path) == 0) return &WEB_FILES[i];
    return nullptr;
}

// Число: "0x.." — шестнадцатеричное, иначе десятичное (ahextoul хаба; "0730" — не восьмеричное)
static long parseNum(const char* s)
{
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtol(s + 2, nullptr, 16);
    return strtol(s, nullptr, 10);
}

static long clampL(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// application/x-www-form-urlencoded: '+' — пробел, %XX — байт (UTF-8 собирается из байтов)
static void urlDecode(char* s)
{
    char* d = s;
    for (; *s; s++) {
        if (*s == '+') { *d++ = ' '; continue; }
        if (*s == '%' && hexVal(s[1]) >= 0 && hexVal(s[2]) >= 0) {
            *d++ = (char)(hexVal(s[1]) * 16 + hexVal(s[2]));
            s += 2;
            continue;
        }
        *d++ = *s;
    }
    *d = '\0';
}

/**
 * @brief Parses a time string in "HHMM" format and converts it to a uint16_t.
 *
 * This function extracts a numeric value from the given string using parseNum(),
 * then validates it as a proper time representation where the hours (HH) are
 * less than 24 and the minutes (MM) are less than 60.
 *
 * @param s A pointer to a null-terminated character string representing the time in "HHMM" format.
 * @return uint16_t The parsed time as a 16-bit unsigned integer if the format and
 *                  values are valid (0 <= HH < 24 and 0 <= MM < 60).
 *                  Returns 0 if the parsed value is negative or if the time is invalid.
 */
static uint16_t parseHhmm(const char* s)
{
    long v = parseNum(s);
    return (v >= 0 && v / 100 < 24 && v % 100 < 60) ? (uint16_t)v : 0;
}

// ------------------------------------------------------------------ запись переменных (web_int_vars.c)
// false — имя неизвестно
static bool setVar(ReqCtx& c, const char* name, const char* val)
{
    const long v = parseNum(val);
    CfgCo2& co2 = Cfg::co2;
    CfgFan& f   = Cfg::fans[c.fan];

    // --- cfg_co2_* ---
    if      (!strcmp(name, "cfg_co2_fans"))             { co2.fans = (uint8_t)clampL(v, 0, FANS_MAX); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_co2_fans_speed_th")) {
        const char* p = val;
        for (uint8_t i = 0; i < FAN_SPEED_MAX && *p; i++) {
            co2.thresholds[i] = (uint16_t)clampL(parseNum(p), 0, UINT16_MAX);
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        c.recalcAll = true;
    }
    else if (!strcmp(name, "cfg_co2_fans_speed_delta")) co2.speedDelta = (uint16_t)clampL(v, 0, UINT16_MAX);
    else if (!strcmp(name, "cfg_co2_period")) {         // период = шаг записей истории: при смене история очищается
        const uint16_t p = (uint16_t)clampL(v, TRANSMIT_PERIOD_MIN_S, TRANSMIT_PERIOD_MAX_S);
        if (p != co2.transmitPeriodS) { co2.transmitPeriodS = p; c.historyResize = true; }   // записей = сутки / период
    }
    else if (!strcmp(name, "cfg_co2_night_start"))      { co2.nightStart   = parseHhmm(val); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_co2_night_end"))        { co2.nightEnd     = parseHhmm(val); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_co2_night_start_wd"))   { co2.nightStartWd = parseHhmm(val); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_co2_night_end_wd"))     { co2.nightEndWd   = parseHhmm(val); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_co2_night_max"))        { co2.nightMaxSpeed = (uint8_t)clampL(v, 0, FAN_SPEED_MAX); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_co2_csv_delim"))        co2.csvDelimiter = (val[0] > ' ' && (uint8_t)val[0] < 0x80) ? val[0] : CSV_DELIMITER_DEF;
    else if (!strcmp(name, "cfg_co2_refresh_t"))        co2.pageRefreshMs = (uint16_t)clampL(v, 0, UINT16_MAX);
    else if (!strcmp(name, "cfg_co2_bright_day"))       { co2.brightDayPct   = (uint8_t)clampL(v, BRIGHT_MIN_PCT, BRIGHT_MAX_PCT); c.saveCo2 = true; }
    else if (!strcmp(name, "cfg_co2_bright_night"))     { co2.brightNightPct = (uint8_t)clampL(v, BRIGHT_MIN_PCT, BRIGHT_MAX_PCT); c.saveCo2 = true; }
    else if (!strcmp(name, "cfg_digits_font"))          { co2.digitsFont = (uint8_t)clampL(v, 0, DIGITS_FONT_COUNT - 1); c.saveCo2 = true; }
    else if (!strcmp(name, "cfg_hist_days")) {         // размер истории, сутки: при смене история пересоздаётся
        const uint16_t d = (uint16_t)clampL(v, HISTORY_DAYS_MIN, HISTORY_DAYS_MAX);
        if (d != co2.historyDays) { co2.historyDays = d; c.historyResize = true; }
    }
    else if (!strcmp(name, "cfg_co2_radio_mode"))       co2.radioMode = (uint8_t)clampL(v, 0, RADIO_MODE_COUNT - 1);   // применяется сразу
    else if (!strcmp(name, "cfg_co2_passive_ch"))       co2.passiveChannel = (uint8_t)clampL(v, 0, RF_CHANNEL_MAX);
    else if (!strcmp(name, "cfg_co2_radio_reset"))      co2.radioResetS = (uint16_t)clampL(v, 0, UINT16_MAX);
    else if (!strcmp(name, "cfg_temp_period"))          co2.tempPeriodS = (uint8_t)clampL(v, TEMP_PERIOD_MIN_S, TEMP_PERIOD_MAX_S);
    else if (!strcmp(name, "cfg_temp_sensor"))          co2.tempSensor  = (uint8_t)clampL(v, 0, TEMP_SENSOR_COUNT - 1);   // DS18B20 без вывода — sanitize() -> нет
    else if (!strcmp(name, "cfg_co2_save"))             c.saveCo2  = v == 1;
    else if (!strcmp(name, "cfg_co2_save_fans"))        { c.saveFans = v == 1; c.recalcAll = true; }
    // --- cfg_fan_* (вентилятор c.fan; FAN_PARAM должен идти раньше полей вентилятора) ---
    else if (!strcmp(name, FAN_PARAM))                  c.fan = (v >= 0 && v < co2.fans) ? (uint8_t)v : 0;
    else if (!strcmp(name, "cfg_fan_name"))             copyUtf8(f.name, val, sizeof(f.name));
    else if (!strcmp(name, "cfg_fan_rf_ch"))            f.rfChannel  = (uint8_t)clampL(v, 0, RF_CHANNEL_MAX);
    else if (!strcmp(name, "cfg_fan_addr_LSB"))         f.addressLsb = (uint8_t)clampL(v, 0, UINT8_MAX);
    else if (!strcmp(name, "cfg_fan_min"))              f.speedMin   = (int8_t)clampL(v, 0, FAN_SPEED_MAX);
    else if (!strcmp(name, "cfg_fan_max"))              f.speedMax   = (int8_t)clampL(v, 0, FAN_SPEED_MAX);
    else if (!strcmp(name, "cfg_fan_override_day"))     f.overrideDay   = (uint8_t)clampL(v, 0, OVR_COUNT - 1);
    else if (!strcmp(name, "cfg_fan_override_night"))   f.overrideNight = (uint8_t)clampL(v, 0, OVR_COUNT - 1);
    else if (!strcmp(name, "cfg_fan_day"))              f.speedDay   = (int8_t)clampL(v, -FAN_SPEED_MAX, FAN_SPEED_MAX);
    else if (!strcmp(name, "cfg_fan_night"))            f.speedNight = (int8_t)clampL(v, -FAN_SPEED_MAX, FAN_SPEED_MAX);
    else if (!strcmp(name, "cfg_fan_flags"))            f.flags = (uint8_t)(v & (FAN_FLAG_SKIP | FAN_FLAG_FORCED));
    else if (!strcmp(name, "cfg_fan_pause"))            f.pauseS   = (uint8_t)clampL(v, 0, UINT8_MAX);
    else if (!strcmp(name, "cfg_fan_timeout"))          f.timeoutS = (uint8_t)clampL(v, 0, UINT8_MAX);
    else if (!strcmp(name, "cfg_fan_override"))         FanControl::fanOverride(c.fan, val[0], (uint16_t)clampL(parseNum(val + (val[0] ? 1 : 0)), 0, UINT16_MAX));
    // --- cfg_vars_*, ночь ---
    else if (!strcmp(name, "cfg_vars_fans_speed_ov"))   { Cfg::vars.speedOverride = (int8_t)clampL(v, -FAN_SPEED_MAX, FAN_SPEED_MAX); c.recalcAll = true; }
    else if (!strcmp(name, "cfg_vars_save"))            c.saveVars = v == 1;
    else if (!strcmp(name, "now_night_ov"))             { Cfg::nightOverride = (uint8_t)clampL(v, 0, NIGHT_OVR_COUNT - 1); c.recalcAll = true; }
    // --- сеть ---
    else if (!strcmp(name, "net_ssid"))                 copyUtf8(Cfg::net.ssid, val, sizeof(Cfg::net.ssid));
    else if (!strcmp(name, "net_pass"))                 { if (val[0]) copyUtf8(Cfg::net.pass, val, sizeof(Cfg::net.pass)); }
    else if (!strcmp(name, "net_ap_ssid"))              { if (val[0]) copyUtf8(Cfg::net.apSsid, val, sizeof(Cfg::net.apSsid)); }
    else if (!strcmp(name, "net_ap_pass"))              { if (strlen(val) >= NET_AP_PASS_MIN) copyUtf8(Cfg::net.apPass, val, sizeof(Cfg::net.apPass)); }   // короче — не меняется
    // пароль настроек: пустое поле — не менять; «без пароля» (идёт в форме после поля) — стереть
    else if (!strcmp(name, "net_web_pass"))             { if (val[0]) copyUtf8(Cfg::net.webPass, val, sizeof(Cfg::net.webPass)); }
    else if (!strcmp(name, "net_web_nopass"))           { if (v == 1) Cfg::net.webPass[0] = '\0'; }
    else if (!strcmp(name, "net_web_save"))             c.saveNet = v == 1;   // без переподключения Wi-Fi
    else if (!strcmp(name, "net_tz"))                   { if (val[0]) copyUtf8(Cfg::net.tz,  val, sizeof(Cfg::net.tz)); }
    else if (!strcmp(name, "net_ntp"))                  { if (val[0]) copyUtf8(Cfg::net.ntp, val, sizeof(Cfg::net.ntp)); }
    else if (!strcmp(name, "net_ap_delay"))             Cfg::net.apDelayMin = (uint16_t)clampL(v, WIFI_AP_DELAY_LO_MIN, WIFI_AP_DELAY_HI_MIN);
    else if (!strcmp(name, "net_ntp_period"))           Cfg::net.ntpPeriodMin = (uint16_t)clampL(v, NTP_PERIOD_LO_MIN, NTP_PERIOD_HI_MIN);
    else if (!strcmp(name, "net_forget"))               { if (v == 1) { Cfg::net.ssid[0] = '\0'; Cfg::net.pass[0] = '\0'; c.saveNet = c.reconnect = true; } }
    else if (!strcmp(name, "net_save"))                 { c.saveNet = v == 1; c.reconnect = c.saveNet; }
    // --- система ---
    else if (!strcmp(name, "sys_touch_recal"))          { if (v == 1) { Cfg::clearTouchCal(); c.restart = true; } }
    else if (!strcmp(name, "sys_time_set"))             Net::setTimeManual((time_t)v);   // UTC, с (от браузера)
    else return false;
    return true;
}

// Разбор "a=b&c=d" (urlencoded), onlyName != nullptr — только эта переменная
static void applyPairs(ReqCtx& c, char* s, const char* onlyName)
{
    while (s && *s) {
        char* next = strchr(s, '&');
        if (next) *next++ = '\0';
        char* eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            char* val = eq + 1;
            urlDecode(s);
            urlDecode(val);
            if (onlyName == nullptr || !strcmp(s, onlyName)) {
                if (!setVar(c, s, val) && c.unknown[0] == '\0') strlcpy(c.unknown, s, sizeof(c.unknown));
            }
        }
        s = next;
    }
}

static void finishVars(ReqCtx& c)
{
    Cfg::sanitize();
    if (c.historyResize)   // срок или период: пересоздать (и очистить) буферы истории
        History::configure(Cfg::co2.historyDays, Cfg::co2.transmitPeriodS, Cfg::co2.tempSensor != TEMP_SENSOR_NONE);
    if (c.recalcAll) FanControl::sendNow(FAN_ALL, true);
    if (c.saveCo2)  Cfg::saveCo2();
    if (c.saveFans) Cfg::saveFans();
    if (c.saveVars) Cfg::saveVars();
    if (c.saveNet)  Cfg::saveNet();
    if (c.reconnect) Net::requestReconnect();
}

// ------------------------------------------------------------------ чтение переменных (web_int_callbacks.c)
// Все переменные страниц одним объектом. Имена — те же, что в setVar (поля форм заполняются по имени).
// ------------------------------------------------------------------ /api/vars по группам
// Страница запрашивает только нужные группы: /api/vars?g=main,fans (атрибут <body data-groups> в web/*.htm);
// без g — все. sys — всегда (подвал, период опроса). Каждая переменная — ровно в одной группе
// (проверяет Scripts/check_web.js: и это, и что странице хватает её групп).
// Группа собирается под CfgLock в s_json и уходит чанком без замка (медленный клиент не держит loop()).
static void varsSys(Json& j, const ReqCtx&)
{
    j.str ("sys_ver",        FW_VERSION);
    j.str ("sys_build",      FW_BUILD_DATE);
    j.str ("sys_author",     FW_AUTHOR);
    j.str ("sys_board",      BOARD_NAME);
    j.str ("sys_fw_board",   FW_BOARD_ID);   // OTA: файл firmware_<плата>_<датчик>_<версия>.bin — проверка на странице
    j.str ("sys_fw_id",      FW_ID);         //   плата_датчик этой прошивки
    j.str ("sys_sensor",     CO2_SENSOR_NAME);
    j.num ("sys_mactime",    (long)Net::bootEpoch());
    j.unum("sys_uptime",     Net::uptimeS());
    j.strf("sys_heap",       "%lu / %lu байт", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    j.num ("sntp_time",      Net::timeValid() ? (long)time(nullptr) : 0L);
    if (!Net::timeValid())                                  j.str("sntp_status", "не установлено");
    else if (Net::timeSource() == Net::TimeSource::Manual)  j.str("sntp_status", "установлено вручную");
    else                                                    j.str("sntp_status", "NTP");
    j.unum("cfg_co2_refresh_t", Cfg::co2.pageRefreshMs);
}

// Главная: CO2, ночь, общая поправка, температура
static void varsMain(Json& j, const ReqCtx&)
{
    j.unum("CO2_current",    FanControl::lastCo2());
    j.num ("CO2_last_time",  (long)Net::epochFromUptime(FanControl::lastCo2UptimeS()));
    j.num ("now_night",      FanControl::isNight() ? 1 : 0);
    j.num ("night",          FanControl::nightEffective() ? 1 : 0);
    j.num ("now_night_ov",   Cfg::nightOverride);
    j.num ("fan_speed_previous", FanControl::speedPrevious());
    j.unum("fsp_time_def",   FORCE_MINUTES_DEF);
    j.num ("cfg_vars_fans_speed_ov", Cfg::vars.speedOverride);
    // датчик температуры (temp_en = 0 — не выбран: строки на главной скрываются)
    j.num ("temp_en",        Cfg::co2.tempSensor != TEMP_SENSOR_NONE ? 1 : 0);
    if (TempSensor::valid() && TempSensor::humidity() >= 0) j.num("temp_rh", TempSensor::humidity());
    else                                                      j.str("temp_rh", "");
    if (TempSensor::valid()) {
        const int16_t t = TempSensor::tenthsC(), a = t < 0 ? -t : t;
        j.strf("temp_c",     "%s%d.%d", t < 0 ? "-" : "", a / TENTHS_PER_DEG, a % TENTHS_PER_DEG);
    } else {
        j.str ("temp_c",     "--.-");
    }
}

// Состояние вентиляторов (fans.xml хаба)
static void varsFans(Json& j, const ReqCtx&)
{
    const CfgCo2& co2 = Cfg::co2;
    j.beginArr("fans");
    for (uint8_t i = 0; i < co2.fans; i++) {
        const CfgFan&   fi = Cfg::fans[i];
        const FanState& st = FanControl::state(i);
        j.beginObj();
        j.str ("name", fi.name);
        j.unum("fl",   fi.flags);
        j.unum("fspt", st.forcedTimeoutS);   // до конца принудительной скорости, с (0 — без ограничения)
        j.unum("spc",  st.speedCurrent);
        j.unum("tst",  st.txStatus);         // > 1 — ошибка передачи
        j.num ("ttm",  (long)Net::epochFromUptime(st.txOkUptimeS));
        if (co2.radioMode == RADIO_PASSIVE) {   // состояние, сообщённое вентилятором (пассивный режим)
            j.unum("rst", st.remoteStatus);
            j.num ("adj", st.remoteAdjust);
            j.boolean("off", st.remoteOff);
            j.boolean("eep", st.remoteEeprom);
        }
        j.endObj();
    }
    j.endArr();
}

// Радио: режим связи и канал пассивного режима
static void varsRadio(Json& j, const ReqCtx&)
{
    j.unum("cfg_co2_radio_mode",  Cfg::co2.radioMode);
    j.unum("cfg_co2_passive_ch",  Cfg::co2.passiveChannel);
    j.unum("cfg_co2_radio_reset", Cfg::co2.radioResetS);
    j.unum("radio_rx_count",      FanControl::passiveRxCount());
}

// Настройки CO2, ночи, экрана, датчика температуры
static void varsCfg(Json& j, const ReqCtx&)
{
    const CfgCo2& co2 = Cfg::co2;
    char tmp[LINE_MAX_LEN];
    j.unum("cfg_co2_fans",   co2.fans);
    size_t n = 0;
    tmp[0] = '\0';
    for (uint8_t i = 0; i < FAN_SPEED_MAX && n < sizeof(tmp); i++)
        n += snprintf(tmp + n, sizeof(tmp) - n, "%u%s", co2.thresholds[i], i < FAN_SPEED_MAX - 1 ? "," : "");
    j.str ("cfg_co2_fans_speed_th",    tmp);
    j.unum("cfg_co2_fans_speed_delta", co2.speedDelta);
    j.strf("cfg_co2_night_start",      "%04u", co2.nightStart);
    j.strf("cfg_co2_night_end",        "%04u", co2.nightEnd);
    j.strf("cfg_co2_night_start_wd",   "%04u", co2.nightStartWd);
    j.strf("cfg_co2_night_end_wd",     "%04u", co2.nightEndWd);
    j.unum("cfg_co2_night_max",        co2.nightMaxSpeed);
    j.unum("cfg_co2_bright_day",       co2.brightDayPct);
    j.unum("cfg_co2_bright_night",     co2.brightNightPct);
    j.unum("cfg_digits_font",          co2.digitsFont);
    j.num ("temp_ds_ok",               TEMP_DS18B20_AVAILABLE ? 1 : 0);   // есть вывод под DS18B20
    j.unum("cfg_temp_sensor",          co2.tempSensor);
    j.unum("cfg_temp_period",          co2.tempPeriodS);
}

// История: буферы (фактически выделено, записей, лимит памяти), срок, период, разделитель CSV
static void varsHist(Json& j, const ReqCtx&)
{
    const CfgCo2& co2 = Cfg::co2;
    char tmp[2] = { co2.csvDelimiter, '\0' };
    j.unum("history_count",   History::count());
    j.unum("hist_cap",        History::capacity());
    j.unum("hist_wanted",     History::wanted());
    j.unum("hist_max",        History::maxRecords());
    j.unum("hist_co2_bytes",  History::co2Bytes());
    j.unum("hist_temp_bytes", History::tempBytes());
    j.unum("hist_temp_count", History::tempCount());
    j.num ("hist_psram",      History::inPsram() ? 1 : 0);
    j.unum("hist_rec_bytes",  HISTORY_CO2_REC_BYTES + (co2.tempSensor != TEMP_SENSOR_NONE ? HISTORY_TEMP_REC_BYTES : 0));
    j.unum("cfg_hist_days",   co2.historyDays);
    j.unum("cfg_co2_period",  co2.transmitPeriodS);
    j.str ("cfg_co2_csv_delim", tmp);
}

// Процессор: загрузка ядер, температура кристалла, задачи FreeRTOS
static void varsCpu(Json& j, const ReqCtx&)
{
    j.unum("cpu_load0",       SysInfo::cpuLoad(0));
    j.unum("cpu_load1",       SysInfo::cpuLoad(1));
    if (SysInfo::chipTempValid()) {
        const int16_t ct = SysInfo::chipTempTenths(), ca = ct < 0 ? -ct : ct;
        j.strf("chip_temp",   "%s%d.%d", ct < 0 ? "-" : "", ca / TENTHS_PER_DEG, ca % TENTHS_PER_DEG);
    } else {
        j.str ("chip_temp",   "--");
    }
    // задачи: [имя, состояние (SysInfo::TaskState), приоритет, ядро (-1 — любое),
    //          мин. свободный стек (байт), время с запуска (мс), доля ЦП с запуска (0,1 %)]
    j.unum("rtos_stack_warn", TASK_STACK_WARN_B);
    j.num ("rtos_overflow",   SysInfo::taskOverflow() ? 1 : 0);
    j.beginArr("rtos_tasks");
    for (uint8_t i = 0; i < SysInfo::taskCount(); i++) {
        const SysInfo::TaskRow* t = SysInfo::task(i);
        j.beginArr(nullptr);
        j.str (nullptr, t->name);
        j.unum(nullptr, t->state);
        j.unum(nullptr, t->prio);
        j.num (nullptr, t->core);
        j.unum(nullptr, t->stackMinB);
        j.u64 (nullptr, t->timeUs / US_PER_MS);
        j.unum(nullptr, t->shareTenths);
        j.endArr();
    }
    j.endArr();
}

// Строки состояния датчиков и радио (страница настроек)
static void varsStatus(Json& j, const ReqCtx&)
{
    char tmp[LINE_MAX_LEN];
    Co2Sensor::statusText(tmp, sizeof(tmp));
    j.str ("co2_status",  tmp);
    TempSensor::statusText(tmp, sizeof(tmp));
    j.str ("temp_status", tmp);
    j.str ("radio_regs",  s_radioReport);
}

// Сеть и часы
static void varsNet(Json& j, const ReqCtx&)
{
    char tmp[LINE_MAX_LEN];
    j.str ("net_ssid", Cfg::net.ssid);
    j.str ("net_tz",   Cfg::net.tz);
    j.str ("net_ntp",  Cfg::net.ntp);
    Net::ipString(tmp, sizeof(tmp));
    j.str ("net_ip",   tmp);
    j.num ("net_rssi", Net::rssi());
    j.unum("net_ap_delay", Cfg::net.apDelayMin);
    j.unum("net_ntp_period", Cfg::net.ntpPeriodMin);
    j.str ("net_ap_ssid", Cfg::net.apSsid);   // пароли (роутера, точки доступа, веба) не отдаются
    const char* ap = Net::apSsid();           // применённое имя (меняется под CfgLock — как и это чтение)
    const uint32_t offS = Net::offlineS(), apS = (uint32_t)Cfg::net.apDelayMin * S_PER_MIN;
    switch (Net::mode()) {
    case Net::Mode::Station:      j.str("net_mode", "подключено к Wi-Fi"); break;
    case Net::Mode::Connecting:
        j.strf("net_mode", "нет связи с роутером %lu с, точка доступа %s включится через %lu с",
               (unsigned long)offS, ap, (unsigned long)(apS > offS ? apS - offS : 0));
        break;
    case Net::Mode::AccessPoint:  j.strf("net_mode", "точка доступа %s (сеть не задана)", ap); break;
    case Net::Mode::StationAndAp:
        if (Net::apClients())
            j.strf("net_mode", "точка доступа %s + поиск роутера приостановлен: к точке доступа "
                   "подключены (нет связи %lu с)", ap, (unsigned long)offS);
        else
            j.strf("net_mode", "точка доступа %s + поиск роутера раз в %lu мин (нет связи %lu с)",
                   ap, (unsigned long)(WIFI_RETRY_MS / MS_PER_MIN), (unsigned long)offS);
        break;
    default:                      j.str("net_mode", "Wi-Fi выключен"); break;
    }
}

// Выбранный вентилятор (?cfg_fan_=N)
static void varsFan(Json& j, const ReqCtx& c)
{
    const CfgFan& f = Cfg::fans[c.fan];
    j.unum("cfg_fan_idx",            c.fan);
    j.str ("cfg_fan_name",           f.name);
    j.unum("cfg_fan_rf_ch",          f.rfChannel);
    j.strf("cfg_fan_addr_LSB",       "0x%02X", f.addressLsb);
    j.num ("cfg_fan_min",            f.speedMin);
    j.num ("cfg_fan_max",            f.speedMax);
    j.unum("cfg_fan_override_day",   f.overrideDay);
    j.unum("cfg_fan_override_night", f.overrideNight);
    j.num ("cfg_fan_day",            f.speedDay);
    j.num ("cfg_fan_night",          f.speedNight);
    j.unum("cfg_fan_flags",          f.flags);
    j.unum("cfg_fan_pause",          f.pauseS);
    j.unum("cfg_fan_timeout",        f.timeoutS);
}

struct VarGroup {
    const char* name;
    void (*write)(Json& j, const ReqCtx& c);
};
// Порядок — порядок в ответе; sys — первой (всегда)
static const VarGroup VAR_GROUPS[] = {
    { "sys",    varsSys    },
    { "main",   varsMain   },
    { "fans",   varsFans   },
    { "radio",  varsRadio  },
    { "cfg",    varsCfg    },
    { "hist",   varsHist   },
    { "cpu",    varsCpu    },
    { "status", varsStatus },
    { "net",    varsNet    },
    { "fan",    varsFan    },
};
constexpr uint8_t VAR_GROUP_COUNT = sizeof(VAR_GROUPS) / sizeof(VAR_GROUPS[0]);
static_assert(VAR_GROUP_COUNT <= 16, "маска групп — uint16_t");
constexpr uint16_t VAR_GROUPS_ALL = (uint16_t)((1u << VAR_GROUP_COUNT) - 1);
constexpr uint16_t VAR_GROUP_SYS  = 1u << 0;

// «main,fans» -> маска; неизвестные имена пропускаются (их ловит check_web.js)
static uint16_t parseGroups(char* s)
{
    uint16_t mask = 0;
    char* save = nullptr;
    for (char* p = strtok_r(s, GROUPS_SEP, &save); p; p = strtok_r(nullptr, GROUPS_SEP, &save))
        for (uint8_t i = 0; i < VAR_GROUP_COUNT; i++)
            if (!strcmp(p, VAR_GROUPS[i].name)) mask |= (uint16_t)(1u << i);
    return mask;
}

// history.csv: от новой записи к старой, "yyyy-mm-dd hh:mm:ss<разд.>ppm<разд.>температура" (web_get_history хаба
// + столбец temp: °C с точкой, пусто — нет данных)
static void sendHistory(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", CACHE_NONE);
    Out o(req);
    char delim;
    { CfgLock l; delim = Cfg::co2.csvDelimiter; }
    o.printf("date%cvalue%ctemp\r\n", delim, delim);
    for (uint32_t i = 0;; i++) {
        HistoryRecord rec;
        bool ok;
        int16_t tt;
        { CfgLock l; ok = History::get(i, rec); tt = History::getTemp(i); }
        if (!ok) break;
        time_t t = Net::epochFromUptime(rec.uptimeS);
        if (t == 0) break;   // время неизвестно — дальше только более старые записи
        struct tm tm;
        localtime_r(&t, &tm);
        char temp[12] = "";
        if (tt != HISTORY_NO_TEMP) {
            const int16_t a = tt < 0 ? -tt : tt;
            snprintf(temp, sizeof(temp), "%s%d.%d", tt < 0 ? "-" : "", a / TENTHS_PER_DEG, a % TENTHS_PER_DEG);
        }
        o.printf("%04d-%02d-%02d %02d:%02d:%02d%c%u%c%s\r\n", 1900 + tm.tm_year, 1 + tm.tm_mon, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, delim, rec.co2, delim, temp);
    }
    o.end();
}

static bool readBody(httpd_req_t* req)
{
    size_t len = req->content_len;
    if (len > WEB_POST_MAX) return false;
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, s_body + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return false;
        got += (size_t)r;
    }
    s_body[got] = '\0';
    return true;
}

static esp_err_t sendJson(httpd_req_t* req, const char* data, size_t len);

// ------------------------------------------------------------------ пароль настроек (HTTP Basic)
// Пароль пустой — доступ без пароля. Иначе «Authorization: Basic base64(имя:пароль)», имя — любое.
static bool authorized(httpd_req_t* req)
{
    char want[sizeof(Cfg::net.webPass)];
    { CfgLock l; strlcpy(want, Cfg::net.webPass, sizeof(want)); }
    if (want[0] == '\0') return true;
    char hdr[AUTH_HDR_MAX];
    if (httpd_req_get_hdr_value_str(req, AUTH_HDR, hdr, sizeof(hdr)) != ESP_OK) return false;
    const size_t pre = strlen(AUTH_BASIC);
    if (strncmp(hdr, AUTH_BASIC, pre) != 0) return false;
    unsigned char dec[AUTH_DEC_MAX];
    size_t n = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char*)hdr + pre, strlen(hdr + pre)) != 0)
        return false;
    dec[n] = '\0';
    const char* colon = strchr((const char*)dec, ':');
    return colon && strcmp(colon + 1, want) == 0;
}

static esp_err_t askAuth(httpd_req_t* req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"" NET_WEB_REALM "\"");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "Нужен пароль настроек", HTTPD_RESP_USE_STRLEN);
}

// Все имена запроса /api/set — из OPEN_VARS (управление с главной)? Имена — ASCII, без декодирования.
static bool onlyOpenVars(const char* s)
{
    while (*s) {
        const size_t len = strcspn(s, "=&");
        bool open = false;
        for (const char* o : OPEN_VARS)
            if (strlen(o) == len && !strncmp(s, o, len)) { open = true; break; }
        if (!open) return false;
        s += len;
        s += strcspn(s, "&");
        if (*s == '&') s++;
    }
    return true;
}

static bool isAuthPage(const char* path)
{
    for (const char* p : AUTH_PAGES)
        if (!strcmp(path, p)) return true;
    return false;
}

// ------------------------------------------------------------------ обновление прошивки (OTA)
// Тело запроса пишется во второй раздел приложения (app0 / app1) по кускам s_json; образ проверяет
// esp_ota_end() (заголовок, чип, контрольная сумма). Новый раздел загрузочный — перезапуск; при старте
// фреймворк подтверждает образ (CONFIG_APP_ROLLBACK_ENABLE, verifyOta()), не запустившийся — откатывается.
static esp_err_t otaFail(httpd_req_t* req, esp_ota_handle_t h, const char* msg)
{
    if (h) esp_ota_abort(h);
    Serial.printf("OTA: %s\n", msg);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
}

static esp_err_t apiOta(httpd_req_t* req)
{
    if (req->method != HTTP_POST) return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST");
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) return otaFail(req, 0, "нет раздела для обновления");
    if (req->content_len == 0 || req->content_len > part->size) return otaFail(req, 0, "размер файла не подходит");
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, req->content_len, &h) != ESP_OK) return otaFail(req, 0, "esp_ota_begin");
    Serial.printf("OTA: %u байт -> %s\n", (unsigned)req->content_len, part->label);
    size_t left = req->content_len;
    while (left) {
        const int r = httpd_req_recv(req, s_json, left < sizeof(s_json) ? left : sizeof(s_json));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return otaFail(req, h, "приём прерван");
        if (esp_ota_write(h, s_json, (size_t)r) != ESP_OK) return otaFail(req, h, "не образ прошивки ESP32");
        left -= (size_t)r;
    }
    if (esp_ota_end(h) != ESP_OK) return otaFail(req, 0, "образ повреждён или для другого чипа");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return otaFail(req, 0, "esp_ota_set_boot_partition");
    Serial.println("OTA: готово, перезапуск");
    char resp[JSON_RESP_LEN];
    Json j(resp, sizeof(resp));
    j.beginObj();
    j.boolean("ok", true);
    j.boolean("restart", true);
    j.endObj();
    sendJson(req, resp, j.length());
    vTaskDelay(pdMS_TO_TICKS(WEB_RESTART_DELAY_MS));
    ESP.restart();
    return ESP_OK;
}

static esp_err_t sendJson(httpd_req_t* req, const char* data, size_t len)
{
    httpd_resp_set_type(req, MIME_JSON);
    httpd_resp_set_hdr(req, "Cache-Control", CACHE_NONE);
    return httpd_resp_send(req, data, len);
}

// Ответ — один JSON-объект, отправляется чанками по группам: «{» + группа, «,» + группа …, «}».
// s_json[0] — место под «{» / «,», группа пишется с s_json[1]. Группа не влезла в WEB_JSON_MAX — вместо неё
// "json_overflow":"<группа>" (app.js показывает ошибку), остальные группы уходят как обычно.
static esp_err_t apiVars(httpd_req_t* req)
{
    ReqCtx c = {};
    uint16_t mask = 0;
    char groups[VARS_GROUPS_ARG_MAX];
    if (httpd_req_get_url_query_str(req, s_body, sizeof(s_body)) == ESP_OK) {
        if (httpd_query_key_value(s_body, GROUPS_PARAM, groups, sizeof(groups)) == ESP_OK) {
            urlDecode(groups);
            mask = parseGroups(groups);
        }
        CfgLock l;
        applyPairs(c, s_body, FAN_PARAM);   // выбранный вентилятор (проверяется по числу вентиляторов)
    }
    if (mask == 0) mask = VAR_GROUPS_ALL;
    mask |= VAR_GROUP_SYS;

    httpd_resp_set_type(req, MIME_JSON);
    httpd_resp_set_hdr(req, "Cache-Control", CACHE_NONE);
    bool first = true;
    esp_err_t r = ESP_OK;
    for (uint8_t i = 0; i < VAR_GROUP_COUNT && r == ESP_OK; i++) {
        if (!(mask & (1u << i))) continue;
        size_t len;
        {
            CfgLock l;
            Json j(s_json + 1, sizeof(s_json) - 1);
            VAR_GROUPS[i].write(j, c);
            len = j.length();
            if (j.overflow()) {
                Json e(s_json + 1, sizeof(s_json) - 1);
                e.str("json_overflow", VAR_GROUPS[i].name);
                len = e.length();
            }
        }
        if (len == 0) continue;
        s_json[0] = first ? '{' : ',';
        first = false;
        r = httpd_resp_send_chunk(req, s_json, len + 1);
    }
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, first ? "{}" : "}", HTTPD_RESP_USE_STRLEN);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, nullptr, 0);
    return r;
}

static esp_err_t apiSet(httpd_req_t* req)
{
    ReqCtx c = {};
    if (req->method == HTTP_POST) {
        if (!readBody(req)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    } else if (httpd_req_get_url_query_str(req, s_body, sizeof(s_body)) != ESP_OK) {
        s_body[0] = '\0';
    }
    if (!onlyOpenVars(s_body) && !authorized(req)) return askAuth(req);   // настройки — по паролю
    {
        CfgLock l;
        applyPairs(c, s_body, nullptr);
        finishVars(c);
    }
    char resp[JSON_RESP_LEN];
    Json j(resp, sizeof(resp));
    j.beginObj();
    j.boolean("ok", true);
    if (c.unknown[0]) j.str("unknown", c.unknown);
    j.boolean("restart", c.restart);
    j.endObj();
    esp_err_t res = sendJson(req, resp, j.length());
    if (c.restart) {   // перекалибровка тача: ответ отправлен — перезапуск (задача веб-сервера)
        vTaskDelay(pdMS_TO_TICKS(WEB_RESTART_DELAY_MS));
        ESP.restart();
    }
    return res;
}

static esp_err_t handler(httpd_req_t* req)
{
    // Путь без '/' и без строки запроса
    char path[PATH_MAX_LEN];
    const char* uri = req->uri[0] == '/' ? req->uri + 1 : req->uri;
    size_t plen = strcspn(uri, "?");
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, uri, plen);
    path[plen] = '\0';
    if (path[0] == '\0') strlcpy(path, INDEX_FILE, sizeof(path));

    if (!strcmp(path, API_SET))  return apiSet(req);
    if (!strcmp(path, API_OTA))  return authorized(req) ? apiOta(req) : askAuth(req);
    if (isAuthPage(path) && !authorized(req)) return askAuth(req);
    if (req->method == HTTP_POST) return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "POST: /api/set");
    if (!strcmp(path, API_VARS)) return apiVars(req);
    if (!strcmp(path, HISTORY_CSV)) { sendHistory(req); return ESP_OK; }

    const WebFile* f = findFile(path);
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    httpd_resp_set_type(req, f->mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", f->html ? CACHE_NONE : CACHE_STATIC);
    return httpd_resp_send(req, (const char*)f->data, f->len);
}

void begin()
{
    static bool tried = false;                   // запуск при первом включении Wi-Fi (main.cpp), одна попытка:
    if (tried) return;                           // ошибка httpd_start не повторяется на каждом проходе loop()
    tried = true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = WEB_STACK_SIZE;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) return;
    static const httpd_uri_t getUri  = { "/*", HTTP_GET,  handler, nullptr };
    static const httpd_uri_t postUri = { "/*", HTTP_POST, handler, nullptr };
    httpd_register_uri_handler(s_server, &getUri);
    httpd_register_uri_handler(s_server, &postUri);
}

void setRadioReport(const char* report)
{
    strlcpy(s_radioReport, report, sizeof(s_radioReport));
}

} // namespace Web
