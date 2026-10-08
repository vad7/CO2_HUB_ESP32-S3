// net.cpp — Wi-Fi + NTP (замена SNTP хаба).
// Режим Wi-Fi — только в меню экрана (net.wifiMode, по умолчанию «выкл»):
//   выкл  — радио Wi-Fi выключено (WIFI_OFF), веба и NTP нет;
//   часы  — Wi-Fi только для времени: окно синхронизации (станция -> роутер -> NTP, не дольше
//           WIFI_CLOCK_WINDOW_MS), затем радио выключается до следующего окна: после удачи — через
//           net.ntpPeriodMin, после неудачи — через WIFI_CLOCK_RETRY_MS. Точки доступа и веба нет; нужен SSID
//           роутера (задаётся в вебе в режиме «вкл»);
//   вкл   — описано ниже.
// Включён: SSID не задан — только точка доступа net.apSsid / net.apPass (по умолчанию NET_AP_SSID_DEF /
// NET_AP_PASS_DEF, меняются в вебе). SSID задан — станция; если связи с роутером
// нет (не подключилось после старта/смены настроек или пропала) дольше net.apDelayMin минут —
// дополнительно точка доступа (AP+STA) для настройки через веб.
// Поиск роутера — только по своему таймеру: при потере связи сразу, затем раз в WIFI_RETRY_MS (1 мин).
// Автоповтор ядра выключен: он искал бы роутер непрерывно, а при поиске по каналам точка доступа
// переключается вместе со станцией и её клиенты теряют связь. Поэтому, пока к точке доступа кто-то
// подключён, поиск не запускается, а начатая попытка прерывается.
// Связь появилась — AP выключается; при следующей потере связи отсчёт начинается заново.
// Отменить подключение — «Забыть сеть» в вебе (SSID стирается -> только AP).
#include "net.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <stdio.h>
#include "config.h"
#include "hub_config.h"

namespace Net {

constexpr int64_t  US_PER_S        = 1000000;
constexpr uint32_t MS_PER_S        = 1000;

static Mode     s_mode        = Mode::Off;
static volatile TimeSource s_timeSrc = TimeSource::None;   // пишут колбэк SNTP (задача lwIP) и веб-задача

static bool     s_reconnect   = false;
static bool     s_mdnsStarted = false;
static bool     s_staWanted   = false;     // SSID задан — нужна связь с роутером
static uint32_t s_offlineT0   = 0;         // с какого момента нет связи с роутером (millis)
static uint32_t s_beginT      = 0;         // последняя попытка подключения (millis)
static uint32_t s_lastApChk   = 0;         // последняя проверка клиентов точки доступа (millis)
static bool     s_apClients   = false;     // к точке доступа подключён хотя бы один клиент
static volatile uint32_t s_offlineS = 0;   // для веба: сколько секунд нет связи (0 — есть или не нужна)
static char     s_ssid[sizeof(Cfg::net.ssid)];
static char     s_pass[sizeof(Cfg::net.pass)];
static char     s_apSsid[sizeof(Cfg::net.apSsid)];
static char     s_apPass[sizeof(Cfg::net.apPass)];
static uint8_t  s_wifiMode    = WIFI_MODE_OFF;   // net.wifiMode на момент последнего startStation()
// режим «Часы»
static volatile bool s_ntpSynced = false;  // колбэк SNTP (задача lwIP): время получено
static bool     s_clockWindow = false;     // окно синхронизации открыто (радио включено)
static bool     s_clockSntp   = false;     // в этом окне SNTP уже запущен (после подключения к роутеру)
static uint32_t s_clockT0     = 0;         // начало окна (millis)
static uint32_t s_clockNext   = 0;         // начало следующего окна (millis)
static uint32_t s_clockOkUpS  = 0;         // аптайм последней удачной синхронизации (0 — не было)
static char     s_tz[sizeof(Cfg::net.tz)];
static char     s_ntp[sizeof(Cfg::net.ntp)];   // SNTP хранит указатель на имя сервера — копия живёт здесь

static void apOn()
{
    WiFi.mode(s_staWanted ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(s_apSsid, s_apPass);
    s_mode = s_staWanted ? Mode::StationAndAp : Mode::AccessPoint;
}

static void wifiOff()
{
    if (s_mdnsStarted) { MDNS.end(); s_mdnsStarted = false; }
    if (s_mode != Mode::Off) {   // при старте радио ещё не запускалось — выключать нечего
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    s_mode = Mode::Off;
    s_staWanted = false;
    s_apClients = false;
    s_offlineS = 0;
}

static void apOff()
{
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

static void staBegin(uint32_t now)
{
    WiFi.begin(s_ssid, s_pass);
    s_beginT = now;
}

// Старт и смена настроек. Если точка доступа уже работает (через неё и меняют настройки) — она
// остаётся до появления связи с роутером, чтобы при ошибке в SSID/пароле не потерять доступ.
static void startStation(uint32_t now)
{
    uint16_t ntpPeriodMin;
    {
        CfgLock l;
        s_wifiMode = Cfg::net.wifiMode;
        strlcpy(s_ssid, Cfg::net.ssid, sizeof(s_ssid));
        strlcpy(s_pass, Cfg::net.pass, sizeof(s_pass));
        strlcpy(s_apSsid, Cfg::net.apSsid, sizeof(s_apSsid));
        strlcpy(s_apPass, Cfg::net.apPass, sizeof(s_apPass));
        strlcpy(s_tz, Cfg::net.tz, sizeof(s_tz));
        strlcpy(s_ntp, Cfg::net.ntp, sizeof(s_ntp));
        ntpPeriodMin = Cfg::net.ntpPeriodMin;
    }
    setenv("TZ", s_tz, 1);   // часовой пояс — в любом режиме (местное время на экране)
    tzset();
    s_clockWindow = false;
    if (s_wifiMode != WIFI_MODE_ON) {
        if (esp_sntp_enabled()) esp_sntp_stop();   // до первого включения сети SNTP не запускался
        wifiOff();
        if (s_wifiMode == WIFI_MODE_CLOCK) {
            s_clockNext = now;   // первое окно — сразу
            Serial.println("Wi-Fi: режим «Часы» — только синхронизация времени");
        } else {
            Serial.println("Wi-Fi: выключен (меню экрана)");
        }
        return;
    }
    // SNTP: первая синхронизация — сразу, как только есть сеть, дальше — раз в ntpPeriodMin
    // (интервал задаётся до sntp_init внутри configTzTime)
    sntp_set_sync_interval((uint32_t)ntpPeriodMin * MS_PER_MIN);
    configTzTime(s_tz, s_ntp);
    const bool apActive = s_mode == Mode::AccessPoint || s_mode == Mode::StationAndAp;
    s_staWanted = s_ssid[0] != '\0';
    WiFi.disconnect();
    s_offlineT0 = now;
    if (!s_staWanted) {
        apOn();
        Serial.printf("Wi-Fi: сеть не задана — только точка доступа %s\n", s_apSsid);
        return;
    }
    WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(NET_HOSTNAME);
    staBegin(now);
    s_mode = apActive ? Mode::StationAndAp : Mode::Connecting;
    Serial.printf("Wi-Fi: подключение к \"%s\"\n", s_ssid);
}

static void onNtpSync(struct timeval*) { s_timeSrc = TimeSource::Ntp; s_ntpSynced = true; }

// Режим «Часы»: окно синхронизации. Открыть — станция к роутеру; подключились — SNTP (запрос сразу);
// время пришло или окно истекло — радио выключить и назначить следующее окно.
static void clockUpdate(uint32_t now)
{
    if (!s_clockWindow) {
        if ((int32_t)(now - s_clockNext) < 0) return;
        if (s_ssid[0] == '\0') {                       // роутер не задан — синхронизироваться не с кем
            s_clockNext = now + WIFI_CLOCK_RETRY_MS;
            return;
        }
        s_clockWindow = true;
        s_clockSntp = false;
        s_ntpSynced = false;
        s_clockT0 = now;
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(NET_HOSTNAME);
        staBegin(now);
        s_mode = Mode::Connecting;
        Serial.printf("Wi-Fi (часы): подключение к \"%s\" для NTP\n", s_ssid);
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        s_mode = Mode::Station;
        if (!s_clockSntp) {                            // сеть есть — запрос времени сразу
            s_clockSntp = true;
            configTzTime(s_tz, s_ntp);
        }
    }
    const bool ok = s_ntpSynced;
    if (!ok && now - s_clockT0 < WIFI_CLOCK_WINDOW_MS) return;
    uint16_t periodMin;
    { CfgLock l; periodMin = Cfg::net.ntpPeriodMin; }
    if (esp_sntp_enabled()) esp_sntp_stop();           // между окнами SNTP не нужен (сети нет)
    wifiOff();
    s_clockWindow = false;
    s_clockNext = now + (ok ? (uint32_t)periodMin * MS_PER_MIN : WIFI_CLOCK_RETRY_MS);
    if (ok) s_clockOkUpS = uptimeS();
    Serial.printf("Wi-Fi (часы): %s, радио выключено, следующая попытка через %lu мин\n",
                  ok ? "время получено" : "не удалось", (unsigned long)((s_clockNext - now) / MS_PER_MIN));
}

void begin()
{
    sntp_set_time_sync_notification_cb(onNtpSync);
    WiFi.persistent(false);       // настройки храним сами (hub_config), не в NVS Wi-Fi
    WiFi.setAutoReconnect(false); // повтор — только по таймеру в update() (см. заголовок)
    startStation(millis());
}

void requestReconnect() { s_reconnect = true; }

void update(uint32_t now)
{
    if (s_reconnect) {
        s_reconnect = false;
        startStation(now);
        return;
    }
    if (s_wifiMode == WIFI_MODE_CLOCK) { clockUpdate(now); return; }
    if (s_wifiMode != WIFI_MODE_ON) return;
    const bool connected = s_staWanted && WiFi.status() == WL_CONNECTED;
    if (s_staWanted) {
        if (connected) {
            if (s_mode == Mode::StationAndAp) {          // связь есть — точка доступа больше не нужна
                apOff();
                Serial.println("Wi-Fi: связь с роутером есть, точка доступа выключена");
            }
            s_mode = Mode::Station;
            s_offlineS = 0;
        } else {
            if (s_mode == Mode::Station) {               // связь пропала — начало отсчёта и сразу попытка
                s_mode = Mode::Connecting;
                s_offlineT0 = now;
                Serial.println("Wi-Fi: связь с роутером потеряна");
                staBegin(now);
            }
            uint16_t delayMin;
            { CfgLock l; delayMin = Cfg::net.apDelayMin; }
            if (s_mode == Mode::Connecting && now - s_offlineT0 >= (uint32_t)delayMin * MS_PER_MIN) {
                apOn();
                Serial.printf("Wi-Fi: нет связи %u мин — включена точка доступа %s\n", delayMin, s_apSsid);
            }
            if (s_mode == Mode::StationAndAp && now - s_lastApChk >= WIFI_AP_CHECK_MS) {
                s_lastApChk = now;
                const bool clients = WiFi.softAPgetStationNum() > 0;
                if (clients && !s_apClients) {
                    WiFi.disconnect();                   // прервать начатый поиск — не мешать клиенту AP
                    Serial.println("Wi-Fi: к точке доступа подключились — поиск роутера приостановлен");
                }
                s_apClients = clients;
            } else if (s_mode != Mode::StationAndAp) {
                s_apClients = false;
            }
            if (!s_apClients && now - s_beginT >= WIFI_RETRY_MS) {   // повтор раз в минуту (в т.ч. после AUTH_FAIL)
                WiFi.disconnect();
                staBegin(now);
            }
            s_offlineS = (now - s_offlineT0) / MS_PER_S;
        }
    }
    if (connected && !s_mdnsStarted) {
        s_mdnsStarted = MDNS.begin(NET_HOSTNAME);
        if (s_mdnsStarted) MDNS.addService("http", "tcp", 80);
    }
}

uint32_t offlineS() { return s_offlineS; }
bool     apClients() { return s_apClients; }
bool     apActive()     { return s_mode == Mode::AccessPoint || s_mode == Mode::StationAndAp; }
bool     staEnabled()   { return s_staWanted; }
bool     staConnected() { return s_mode == Mode::Station; }
bool     wifiOn()       { return s_wifiMode == WIFI_MODE_ON; }
uint8_t  wifiMode()     { return s_wifiMode; }
bool     clockWindow()  { return s_clockWindow; }
uint32_t clockLastOkUpS() { return s_clockOkUpS; }
uint32_t clockNextS()
{
    const int32_t ms = (int32_t)(s_clockNext - millis());
    return s_wifiMode != WIFI_MODE_CLOCK || s_clockWindow || ms <= 0 ? 0 : (uint32_t)ms / MS_PER_S;
}
const char* staSsid()   { return s_ssid; }
const char* apSsid()    { return s_apSsid; }
const char* apPass()    { return s_apPass; }

static void ipToString(const IPAddress& ip, char* buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
void apIpString(char* buf, size_t len)  { ipToString(WiFi.softAPIP(), buf, len); }
void staIpString(char* buf, size_t len) { ipToString(WiFi.localIP(), buf, len); }

Mode mode() { return s_mode; }

bool timeValid() { return time(nullptr) >= TIME_VALID_EPOCH; }

TimeSource timeSource() { return s_timeSrc; }

bool setTimeManual(time_t utc)
{
    if (utc < TIME_VALID_EPOCH) return false;
    const struct timeval tv = { utc, 0 };
    if (settimeofday(&tv, nullptr) != 0) return false;
    s_timeSrc = TimeSource::Manual;
    Serial.printf("Время установлено вручную: %ld (UTC)\n", (long)utc);
    return true;
}

uint32_t uptimeS() { return (uint32_t)(esp_timer_get_time() / US_PER_S); }   // 64-бит мкс: без переполнения millis()

// Отсчёт назад от текущих часов: событие было (аптайм сейчас − аптайм события) секунд назад.
// Корректировки часов (NTP, вручную) сразу видны во всей истории — отслеживать их не нужно.
time_t epochFromUptime(uint32_t eventUptimeS)
{
    if (eventUptimeS == 0 || !timeValid()) return 0;
    const uint32_t up  = uptimeS();
    const uint32_t age = up > eventUptimeS ? up - eventUptimeS : 0;
    return time(nullptr) - (time_t)age;
}

time_t bootEpoch()
{
    return timeValid() ? time(nullptr) - (time_t)uptimeS() : 0;
}

void ipString(char* buf, size_t len)
{
    IPAddress ip = (s_mode == Mode::Station || WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
    snprintf(buf, len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

int8_t rssi() { return WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0; }

} // namespace Net
