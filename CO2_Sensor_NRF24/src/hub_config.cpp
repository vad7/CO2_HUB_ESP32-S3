// hub_config.cpp — хранение настроек хаба в NVS (аналог flash_read_cfg / flash_save_cfg хаба)
#include "hub_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Cfg {

CfgCo2     co2;
CfgFan     fans[FANS_MAX];
GlobalVars vars;
NetCfg     net;
uint8_t    nightOverride = NIGHT_AUTO;

static const char* const NVS_NAMESPACE = "co2hub";
static const char* const KEY_CO2  = "co2";
static const char* const KEY_FANS = "fans";
static const char* const KEY_VARS = "vars";
static const char* const KEY_NET  = "net";
static const char* const KEY_TOUCH = "touchcal";


static SemaphoreHandle_t s_mutex = nullptr;
static StaticSemaphore_t s_mutexBuf;

void lock()   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(s_mutex); }

void initFan(uint8_t idx)
{
    CfgFan& f = fans[idx];
    memset(&f, 0, sizeof(f));
    snprintf(f.name, sizeof(f.name), "Вент. %u", idx + 1);
    f.rfChannel  = RF_CHANNEL_DEF;
    f.addressLsb = (uint8_t)(FAN_ADDR_LSB_DEF + idx);
    f.speedMin   = 0;
    f.speedMax   = FAN_SPEED_MAX;   // у хаба после memset было 0 — вентилятор стоял бы всегда
    f.pauseS     = FAN_PAUSE_DEF_S;
    f.timeoutS   = FAN_TIMEOUT_DEF_S;
}

static void defaultsCo2()
{
    memset(&co2, 0, sizeof(co2));
    memcpy(co2.thresholds, CO2_THRESHOLDS_DEF, sizeof(co2.thresholds));
    co2.csvDelimiter    = CSV_DELIMITER_DEF;
    co2.pageRefreshMs   = WEB_REFRESH_DEF_MS;
    co2.historyDays     = HISTORY_DAYS_DEF;
    co2.transmitPeriodS = TRANSMIT_PERIOD_DEF_S;
    co2.brightDayPct    = BRIGHT_DAY_DEF_PCT;
    co2.brightNightPct  = BRIGHT_NIGHT_DEF_PCT;
    co2.radioMode       = RADIO_ACTIVE;
    co2.passiveChannel  = PASSIVE_CHANNEL_DEF;
    co2.radioResetS     = RADIO_RESET_DEF_S;
    co2.tempPeriodS     = TEMP_PERIOD_DEF_S;
    co2.tempSensor      = TEMP_SENSOR_NONE;
    co2.digitsFont      = DIGITS_FONT_SMOOTH;
    co2.debug           = 0;
    co2.nightStart      = NIGHT_START_DEF;
    co2.nightEnd        = NIGHT_END_DEF;
    co2.nightStartWd    = NIGHT_START_WD_DEF;
    co2.nightEndWd      = NIGHT_END_WD_DEF;
}

static void defaultsNet()
{
    memset(&net, 0, sizeof(net));
    strlcpy(net.tz,  NET_TZ_DEF,  sizeof(net.tz));
    strlcpy(net.ntp, NET_NTP_DEF, sizeof(net.ntp));
    net.apDelayMin = WIFI_AP_DELAY_DEF_MIN;
    net.ntpPeriodMin = NTP_PERIOD_DEF_MIN;
    strlcpy(net.apSsid, NET_AP_SSID_DEF, sizeof(net.apSsid));
    strlcpy(net.apPass, NET_AP_PASS_DEF, sizeof(net.apPass));
    net.wifiMode = WIFI_MODE_OFF;   // по умолчанию Wi-Fi выключен
    strlcpy(net.webPass, NET_WEB_PASS_DEF, sizeof(net.webPass));
}

static uint8_t clampPct(uint8_t v)
{
    return v < BRIGHT_MIN_PCT ? BRIGHT_MIN_PCT : (v > BRIGHT_MAX_PCT ? BRIGHT_MAX_PCT : v);
}

static int8_t clampI8(int v, int lo, int hi) { return (int8_t)(v < lo ? lo : (v > hi ? hi : v)); }

static uint16_t sanitizeHhmm(uint16_t v)
{
    return (v / 100 < 24 && v % 100 < 60) ? v : 0;
}

void sanitize()
{
    if (net.apDelayMin < WIFI_AP_DELAY_LO_MIN) net.apDelayMin = WIFI_AP_DELAY_DEF_MIN;   // 0 — не задано
    if (net.apDelayMin > WIFI_AP_DELAY_HI_MIN) net.apDelayMin = WIFI_AP_DELAY_HI_MIN;
    if (net.ntpPeriodMin < NTP_PERIOD_LO_MIN) net.ntpPeriodMin = NTP_PERIOD_DEF_MIN;   // 0 — не задано
    if (net.ntpPeriodMin > NTP_PERIOD_HI_MIN) net.ntpPeriodMin = NTP_PERIOD_HI_MIN;
    net.apSsid[sizeof(net.apSsid) - 1] = '\0';
    net.apPass[sizeof(net.apPass) - 1] = '\0';
    if (net.apSsid[0] == '\0')                 strlcpy(net.apSsid, NET_AP_SSID_DEF, sizeof(net.apSsid));
    if (strlen(net.apPass) < NET_AP_PASS_MIN)  strlcpy(net.apPass, NET_AP_PASS_DEF, sizeof(net.apPass));
    if (net.wifiMode >= WIFI_MODE_COUNT) net.wifiMode = WIFI_MODE_OFF;
    net.webPass[sizeof(net.webPass) - 1] = '\0';   // пустой — настройки веба без пароля
    if (co2.fans > FANS_MAX) co2.fans = FANS_MAX;
    if (co2.nightMaxSpeed > FAN_SPEED_MAX) co2.nightMaxSpeed = FAN_SPEED_MAX;
    co2.nightStart   = sanitizeHhmm(co2.nightStart);
    co2.nightEnd     = sanitizeHhmm(co2.nightEnd);
    co2.nightStartWd = sanitizeHhmm(co2.nightStartWd);
    co2.nightEndWd   = sanitizeHhmm(co2.nightEndWd);
    if (co2.csvDelimiter == '\0') co2.csvDelimiter = CSV_DELIMITER_DEF;
    if (co2.transmitPeriodS < TRANSMIT_PERIOD_MIN_S) co2.transmitPeriodS = TRANSMIT_PERIOD_MIN_S;
    if (co2.transmitPeriodS > TRANSMIT_PERIOD_MAX_S) co2.transmitPeriodS = TRANSMIT_PERIOD_MAX_S;
    if (co2.radioMode >= RADIO_MODE_COUNT) co2.radioMode = RADIO_ACTIVE;
    if (co2.passiveChannel > RF_CHANNEL_MAX) co2.passiveChannel = RF_CHANNEL_MAX;
    for (CfgFan& f : fans) {
        f.name[FAN_NAME_LEN] = '\0';
        if (f.rfChannel > RF_CHANNEL_MAX) f.rfChannel = RF_CHANNEL_MAX;
        f.speedMin   = clampI8(f.speedMin, 0, FAN_SPEED_MAX);
        f.speedMax   = clampI8(f.speedMax, 0, FAN_SPEED_MAX);
        f.speedDay   = clampI8(f.speedDay,   -FAN_SPEED_MAX, FAN_SPEED_MAX);
        f.speedNight = clampI8(f.speedNight, -FAN_SPEED_MAX, FAN_SPEED_MAX);
        if (f.overrideDay   >= OVR_COUNT) f.overrideDay   = OVR_NONE;
        if (f.overrideNight >= OVR_COUNT) f.overrideNight = OVR_NONE;
        f.flags &= FAN_FLAG_SKIP | FAN_FLAG_FORCED;
    }
    vars.speedOverride = clampI8(vars.speedOverride, -FAN_SPEED_MAX, FAN_SPEED_MAX);
    if (nightOverride >= NIGHT_OVR_COUNT) nightOverride = NIGHT_AUTO;
    // 0 — «не задано» (в старом блоке на месте нового поля было выравнивание): берём умолчание
    if (co2.brightDayPct == 0)   co2.brightDayPct   = BRIGHT_DAY_DEF_PCT;
    if (co2.brightNightPct == 0) co2.brightNightPct = BRIGHT_NIGHT_DEF_PCT;
    co2.brightDayPct    = clampPct(co2.brightDayPct);
    co2.brightNightPct  = clampPct(co2.brightNightPct);
    if (co2.tempPeriodS == 0)                co2.tempPeriodS = TEMP_PERIOD_DEF_S;
    if (co2.tempPeriodS > TEMP_PERIOD_MAX_S) co2.tempPeriodS = TEMP_PERIOD_MAX_S;
    if (co2.historyDays < HISTORY_DAYS_MIN || co2.historyDays > HISTORY_DAYS_MAX) co2.historyDays = HISTORY_DAYS_DEF;
    if (co2.tempSensor >= TEMP_SENSOR_COUNT || (co2.tempSensor == TEMP_SENSOR_DS18B20 && !TEMP_DS18B20_AVAILABLE))
        co2.tempSensor = TEMP_SENSOR_NONE;
    if (co2.digitsFont >= DIGITS_FONT_COUNT) co2.digitsFont = DIGITS_FONT_SMOOTH;
    if (co2.debug > 1) co2.debug = 0;
}

// ------------------------------------------------------------------------------------------
// Хранение с размером: [BlobHdr][элемент 0][элемент 1]... — каждый элемент ровно elemSize байт.
// Загрузка поверх умолчаний: min(размер в прошивке, размер в NVS) байт по каждому элементу,
// min(число элементов) — новые поля и новые элементы массива сохраняют значения по умолчанию.
// Блок с другой меткой или неверной длиной не читается — структура остаётся по умолчанию.
// ------------------------------------------------------------------------------------------
struct BlobHdr {
    uint16_t magic;       // BLOB_MAGIC — формат блока
    uint16_t elemSize;    // размер данных одного элемента (CFG_DATA_SIZE) в прошивке, записавшей блок
    uint16_t count;       // число элементов (вентиляторов; для одиночной структуры — 1)
};
static_assert(sizeof(BlobHdr) == 6, "заголовок блока NVS — 6 байт");
// 0xC5A2: заголовок 6 байт (2026-10-07). Прежние форматы (0xC5A1 с заголовком 8 байт, сырые структуры
// до 2026-10-05) не читаются: все настройки, кроме калибровки тача, — по умолчанию.
constexpr uint16_t BLOB_MAGIC   = 0xC5A2;
constexpr size_t   CFG_BLOB_MAX = 1024;          // статический буфер (без динамической памяти); 10 вентиляторов — 448 Б + запас под рост
static uint8_t s_blob[CFG_BLOB_MAX];

static_assert(sizeof(BlobHdr) + CFG_DATA_SIZE(CfgCo2)                <= CFG_BLOB_MAX, "CFG_BLOB_MAX мал для CfgCo2");
static_assert(sizeof(BlobHdr) + CFG_DATA_SIZE(CfgFan) * FANS_MAX     <= CFG_BLOB_MAX, "CFG_BLOB_MAX мал для CfgFan[]");
static_assert(sizeof(BlobHdr) + CFG_DATA_SIZE(NetCfg)                <= CFG_BLOB_MAX, "CFG_BLOB_MAX мал для NetCfg");
static_assert(sizeof(BlobHdr) + CFG_DATA_SIZE(GlobalVars)            <= CFG_BLOB_MAX, "CFG_BLOB_MAX мал для GlobalVars");

static size_t minSz(size_t a, size_t b) { return a < b ? a : b; }

// Загрузка массива структур (stride = sizeof(T), elemSize = CFG_DATA_SIZE(T)) поверх умолчаний
static bool loadArr(Preferences& p, const char* key, void* base, size_t stride, size_t elemSize, size_t count)
{
    size_t len = p.getBytesLength(key);
    if (len < sizeof(BlobHdr) || len > sizeof(s_blob) || p.getBytes(key, s_blob, len) != len) return false;
    BlobHdr h;
    memcpy(&h, s_blob, sizeof(h));
    if (h.magic != BLOB_MAGIC || h.elemSize == 0 || sizeof(h) + (size_t)h.elemSize * h.count != len) return false;
    const uint8_t* data = s_blob + sizeof(h);
    const size_t n = minSz(count, h.count), sz = minSz(elemSize, h.elemSize);
    for (size_t i = 0; i < n; i++)
        memcpy(static_cast<uint8_t*>(base) + i * stride, data + i * h.elemSize, sz);
    return true;
}

static bool saveArr(const char* key, const void* base, size_t stride, size_t elemSize, size_t count)
{
    const size_t len = sizeof(BlobHdr) + elemSize * count;
    if (len > sizeof(s_blob)) return false;
    const BlobHdr h = { BLOB_MAGIC, (uint16_t)elemSize, (uint16_t)count };
    memcpy(s_blob, &h, sizeof(h));
    for (size_t i = 0; i < count; i++)
        memcpy(s_blob + sizeof(h) + i * elemSize, static_cast<const uint8_t*>(base) + i * stride, elemSize);
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    bool ok = p.putBytes(key, s_blob, len) == len;
    p.end();
    return ok;
}

template <typename T> static bool loadCfg(Preferences& p, const char* key, T* obj, size_t count = 1)
{
    return loadArr(p, key, obj, sizeof(T), CFG_DATA_SIZE(T), count);
}

template <typename T> static bool saveCfg(const char* key, const T* obj, size_t count = 1)
{
    return saveArr(key, obj, sizeof(T), CFG_DATA_SIZE(T), count);
}

// Калибровка тача — фиксированный массив, формат прежний (точный размер)
static bool loadBlob(Preferences& p, const char* key, void* dst, size_t len)
{
    return p.getBytesLength(key) == len && p.getBytes(key, dst, len) == len;
}

static bool saveBlob(const char* key, const void* src, size_t len)
{
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    bool ok = p.putBytes(key, src, len) == len;
    p.end();
    return ok;
}

void begin()
{
    s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutexBuf);
    // 1) значения по умолчанию, 2) поверх — сохранённое (сколько есть во флеше)
    defaultsCo2();
    for (uint8_t i = 0; i < FANS_MAX; i++) initFan(i);
    memset(&vars, 0, sizeof(vars));
    defaultsNet();

    Preferences p;
    if (p.begin(NVS_NAMESPACE, true)) {
        loadCfg(p, KEY_CO2,  &co2);
        loadCfg(p, KEY_FANS, fans, FANS_MAX);
        loadCfg(p, KEY_VARS, &vars);
        loadCfg(p, KEY_NET,  &net);
        p.end();
    }
    sanitize();
}

bool loadTouchCal(uint16_t* cal)
{
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, true)) return false;
    bool ok = loadBlob(p, KEY_TOUCH, cal, TOUCH_CAL_LEN * sizeof(uint16_t));
    p.end();
    return ok;
}

bool saveTouchCal(const uint16_t* cal) { return saveBlob(KEY_TOUCH, cal, TOUCH_CAL_LEN * sizeof(uint16_t)); }

void clearTouchCal()
{
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return;
    p.remove(KEY_TOUCH);
    p.end();
}

bool saveCo2()  { CfgLock l; return saveCfg(KEY_CO2,  &co2); }
bool saveFans() { CfgLock l; return saveCfg(KEY_FANS, fans, FANS_MAX); }
bool saveVars() { CfgLock l; return saveCfg(KEY_VARS, &vars); }
bool saveNet()  { CfgLock l; return saveCfg(KEY_NET,  &net); }

void restoreDefaults()
{
    CfgLock l;
    defaultsCo2();
    for (uint8_t i = 0; i < FANS_MAX; i++) initFan(i);
    memset(&vars, 0, sizeof(vars));
    nightOverride = NIGHT_AUTO;
    sanitize();
    saveCo2();
    saveFans();
    saveVars();
}

} // namespace Cfg
