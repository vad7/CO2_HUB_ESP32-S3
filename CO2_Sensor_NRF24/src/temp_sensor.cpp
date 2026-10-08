// temp_sensor.cpp — датчик температуры: выбор драйвера по типу из настроек, период, учёт ошибок.
// Смена типа на лету: старый драйвер освобождает вывод / шину, счётчики и значение сбрасываются.
#include "temp_sensor.h"
#include "temp_backend.h"
#include "config.h"
#include "hub_config.h"
#include <Arduino.h>
#include <stdio.h>

namespace TempSensor {

constexpr uint32_t MS_PER_S = 1000;
constexpr int16_t  TENTHS   = 10;

static uint8_t  s_type      = TEMP_SENSOR_NONE;   // активный драйвер
static bool     s_busy      = false;              // идёт измерение
static bool     s_started   = false;              // первое измерение — сразу после выбора
static uint32_t s_cycleStart = 0;
static int16_t  s_tenths    = 0;
static int8_t   s_rh        = -1;                 // влажность, % (-1 — нет)
static bool     s_everValid = false;
static uint8_t  s_fails     = 0;                  // ошибок подряд
static uint32_t s_errors    = 0;                  // ошибок всего
static TempErr  s_lastErr   = TempErr::NoData;

static void detachCurrent()
{
    if (s_type == TEMP_SENSOR_DS18B20) Ds18b20::detach();
    if (s_type == TEMP_SENSOR_SHT40)   Sht40::detach();
}

static void select(uint8_t type)
{
    detachCurrent();
    s_type = type;
    s_busy = s_started = s_everValid = false;
    s_rh = -1;
    s_fails = 0;
    s_errors = 0;
    s_lastErr = TempErr::NoData;
    if (type == TEMP_SENSOR_DS18B20) Ds18b20::attach();
    if (type == TEMP_SENSOR_SHT40)   Sht40::attach();
}

void update(uint32_t now, uint8_t type, uint8_t periodS)
{
    if (type == TEMP_SENSOR_DS18B20 && !TEMP_DS18B20_AVAILABLE) type = TEMP_SENSOR_NONE;
    if (type != s_type) select(type);
    if (s_type == TEMP_SENSOR_NONE) return;
    if (!s_busy) {
        if (s_started && now - s_cycleStart < (uint32_t)periodS * MS_PER_S) return;
        s_started = s_busy = true;
        s_cycleStart = now;
        if (s_type == TEMP_SENSOR_DS18B20) Ds18b20::start(now);
        else                               Sht40::start(now);
        return;
    }
    TempErr err = TempErr::None;
    int16_t t = 0;
    int8_t  rh = -1;
    const TempPoll r = s_type == TEMP_SENSOR_DS18B20 ? Ds18b20::poll(now, err, t, rh) : Sht40::poll(now, err, t, rh);
    if (r == TempPoll::Busy) return;
    s_busy = false;
    s_lastErr = err;
    if (err == TempErr::None) {
        s_tenths = t;
        s_rh = rh;
        s_everValid = true;
        s_fails = 0;
    } else {
        s_errors++;
        if (s_fails < UINT8_MAX) s_fails++;
    }
}

bool    active()  { return s_type != TEMP_SENSOR_NONE; }
bool    valid()   { return active() && s_everValid && s_fails < TEMP_FAIL_COUNT; }
int16_t tenthsC() { return s_tenths; }
int8_t  humidity() { return s_rh; }

const char* typeName(uint8_t type)
{
    switch (type) {
    case TEMP_SENSOR_DS18B20: return "DS18B20";
    case TEMP_SENSOR_SHT40:   return "SHT40";
    default:                  return "нет";
    }
}

void statusText(char* buf, size_t len)
{
    static const char* const ERR_TEXT[(uint8_t)TempErr::Count] = {
        "OK", "нет данных", "линия DQ в нуле (замыкание / нет подтяжки)", "нет ответа датчика", "ошибка CRC",
        "неверные данные", "нет преобразования (85 °C)", "нет ответа по I2C (адрес 0x44..0x46)" };
    if (!active()) { snprintf(buf, len, "не выбран"); return; }
    char where[24];
    if (s_type == TEMP_SENSOR_DS18B20) snprintf(where, sizeof(where), "DS18B20, GPIO%d", TEMP_PIN_DQ);
    else if (Sht40::address())         snprintf(where, sizeof(where), "SHT40, I2C 0x%02X", Sht40::address());
    else                               snprintf(where, sizeof(where), "SHT40, I2C");
    const int16_t a = s_tenths < 0 ? -s_tenths : s_tenths;
    char val[24];
    if (s_rh >= 0) snprintf(val, sizeof(val), "%s%d.%d °C, %d %%", s_tenths < 0 ? "-" : "", a / TENTHS, a % TENTHS, s_rh);
    else           snprintf(val, sizeof(val), "%s%d.%d °C", s_tenths < 0 ? "-" : "", a / TENTHS, a % TENTHS);
    if (valid() && s_lastErr == TempErr::None)
        snprintf(buf, len, "%s (%s, ошибок %lu)", val, where, (unsigned long)s_errors);
    else if (valid())
        snprintf(buf, len, "%s, последнее чтение: %s (%s, ошибок %lu)", val, ERR_TEXT[(uint8_t)s_lastErr], where,
                 (unsigned long)s_errors);
    else
        snprintf(buf, len, "%s (%s, ошибок %lu)", ERR_TEXT[(uint8_t)s_lastErr], where, (unsigned long)s_errors);
}

} // namespace TempSensor
