// errlog.cpp - журнал ошибок (см. errlog.h): статический кольцевой буфер, без кучи.
#include "errlog.h"
#include <Arduino.h>
#include <stdio.h>
#include <time.h>
#include "config.h"
#include "hub_config.h"
#include "co2_sensor.h"
#include "temp_sensor.h"
#include "temp_backend.h"
#include "net.h"
#include <nRF24L01.h>   // адреса регистров nRF24 (библиотека RF24)

namespace ErrLog {

constexpr uint16_t CAP          = (uint16_t)(ERRLOG_BYTES / sizeof(ErrEntry));
constexpr uint8_t  BITS_PER_BYTE = 8;
constexpr uint8_t  BYTE_MASK    = 0xFF;
constexpr uint32_t S_PER_MIN    = 60;
constexpr uint32_t S_PER_HOUR   = 3600;
constexpr uint32_t S_PER_DAY    = 86400;
static_assert(CAP >= ERRLOG_MERGE_DEPTH, "журнал меньше глубины слияния");
static_assert(ERRLOG_BYTES / sizeof(ErrEntry) <= UINT16_MAX, "индексы и счётчик записей - uint16_t");

static ErrEntry s_log[CAP];                         // кольцевой буфер записей
static uint16_t s_head    = 0;      // куда пишется следующая запись
static uint16_t s_count   = 0;                      // записей в журнале (не больше CAP)
static uint32_t s_changes = 0;                      // счётчик добавлений - экран перерисовывает журнал при изменении

static uint16_t slot(uint16_t i) { return (uint16_t)((s_head + CAP - 1 - i) % CAP); }   // i = 0 - самая новая

void add(ErrDev dev, uint8_t code, uint16_t arg)
{
    const uint32_t now = Net::uptimeS();
    s_changes++;
    const uint16_t depth = s_count < ERRLOG_MERGE_DEPTH ? s_count : ERRLOG_MERGE_DEPTH;
    for (uint16_t i = 0; i < depth; i++) {
        ErrEntry& e = s_log[slot(i)];
        if (e.dev == (uint8_t)dev && e.code == code && e.arg == arg) {   // повтор: счётчик и время последнего раза
            e.lastS = now;
            if (e.count < UINT16_MAX) e.count++;
            return;
        }
    }
    ErrEntry& e = s_log[s_head];
    e = ErrEntry{ now, now, 1, arg, (uint8_t)dev, code };
    s_head = (uint16_t)((s_head + 1) % CAP);
    if (s_count < CAP) s_count++;
    char buf[ERRLOG_LINE_LEN];
    describe((uint8_t)dev, code, arg, buf, sizeof(buf));
    Serial.printf("Ошибка: %s\n", buf);   // без терминала вывод отбрасывается (SERIAL_TX_TIMEOUT_MS = 0)
}

uint16_t count()   { return s_count; }
uint32_t changes() { return s_changes; }

bool get(uint16_t i, ErrEntry& e)
{
    if (i >= s_count) return false;
    e = s_log[slot(i)];
    return true;
}

// Имена регистров nRF24, которые попадают в журнал
static const char* regName(uint8_t reg)
{
    switch (reg) {
    case nRF24L01::CONFIG:     return "CONFIG";
    case nRF24L01::EN_AA:      return "EN_AA";
    case nRF24L01::EN_RXADDR:  return "EN_RXADDR";
    case nRF24L01::SETUP_AW:   return "SETUP_AW";
    case nRF24L01::SETUP_RETR: return "SETUP_RETR";
    case nRF24L01::RF_CH:      return "RF_CH";
    case nRF24L01::RF_SETUP:   return "RF_SETUP";
    case nRF24L01::STATUS:     return "STATUS";
    case nRF24L01::RX_PW_P0:   return "RX_PW_P0";
    case nRF24L01::DYNPD:      return "DYNPD";
    case nRF24L01::FEATURE:    return "FEATURE";
    default:                   return "?";
    }
}

static void co2Text(uint8_t code, uint16_t arg, char* buf, size_t len)
{
    char bits[ERRLOG_LINE_LEN / 2];
    switch ((Co2Err)code) {
    case Co2Err::NoAnswer:  snprintf(buf, len, "CO2 %s: нет ответа", CO2_SENSOR_NAME); break;
    case Co2Err::BadFrame:  snprintf(buf, len, "CO2 %s: неверный ответ (контрольная сумма / формат)", CO2_SENSOR_NAME); break;
    case Co2Err::Exception: snprintf(buf, len, "CO2 %s: исключение Modbus %u", CO2_SENSOR_NAME, arg); break;
    case Co2Err::Sensor:
        Co2Sensor::statusBitsText(arg, bits, sizeof(bits));
        snprintf(buf, len, "CO2 %s: ошибка датчика %s", CO2_SENSOR_NAME, bits);
        break;
    case Co2Err::Stale:     snprintf(buf, len, "CO2 %s: нет данных дольше %u периодов опроса", CO2_SENSOR_NAME, CO2_FAIL_PERIODS); break;
    default:                snprintf(buf, len, "CO2 %s: ошибка %u", CO2_SENSOR_NAME, code); break;
    }
}

static void fanText(uint8_t code, uint16_t arg, char* buf, size_t len)
{
    const uint8_t fan = (uint8_t)(arg >> BITS_PER_BYTE);
    char who[48];
    if (fan < FANS_MAX) snprintf(who, sizeof(who), "Вентилятор %u (%s)", fan + 1, Cfg::fans[fan].name);   // без «»: их нет в шрифте экрана
    else                snprintf(who, sizeof(who), "Вентилятор %u", fan + 1);
    switch ((FanErr)code) {
    case FanErr::NoAck:      snprintf(buf, len, "%s: нет подтверждения (ACK)", who); break;
    case FanErr::NoResponse: snprintf(buf, len, "%s: nRF24 не завершил передачу", who); break;
    case FanErr::AddrFail:   snprintf(buf, len, "%s: адрес не записался в nRF24", who); break;
    case FanErr::Silent:     snprintf(buf, len, "%s: нет запросов дольше тайм-аута", who); break;
    case FanErr::Remote:     snprintf(buf, len, "%s: сообщил ошибку связи (0x%02X)", who, arg & BYTE_MASK); break;
    case FanErr::Eeprom:     snprintf(buf, len, "%s: сбой ячейки EEPROM", who); break;
    default:                 snprintf(buf, len, "%s: ошибка %u", who, code); break;
    }
}

void describe(uint8_t dev, uint8_t code, uint16_t arg, char* buf, size_t len)
{
    switch ((ErrDev)dev) {
    case ErrDev::Co2:
        co2Text(code, arg, buf, len);
        break;
    case ErrDev::Temp:
        snprintf(buf, len, "%s: %s", TempSensor::typeName((uint8_t)arg), TempSensor::errText((TempErr)code));
        break;
    case ErrDev::Radio:
        if ((RadioErr)code == RadioErr::NotFound)
            snprintf(buf, len, "nRF24: модуль не найден (нет ответа по SPI)");
        else
            snprintf(buf, len, "nRF24: %s = 0x%02X%s", regName((uint8_t)(arg >> BITS_PER_BYTE)), arg & BYTE_MASK,
                     (RadioErr)code == RadioErr::RegMismatch ? " - не совпал с заданным" : "");
        break;
    case ErrDev::Fan:
        fanText(code, arg, buf, len);
        break;
    default:
        snprintf(buf, len, "устройство %u: ошибка %u", dev, code);
        break;
    }
}

void timeText(uint32_t uptimeS, bool withDate, char* buf, size_t len)
{
    const time_t t = Net::epochFromUptime(uptimeS);
    if (t) {
        struct tm tm;
        localtime_r(&t, &tm);
        if (withDate) snprintf(buf, len, "%02d.%02d %02d:%02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min, tm.tm_sec);
        else          snprintf(buf, len, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(buf, len, "+%lu:%02lu:%02lu", (unsigned long)(uptimeS / S_PER_HOUR),
                 (unsigned long)(uptimeS % S_PER_HOUR / S_PER_MIN), (unsigned long)(uptimeS % S_PER_MIN));
    }
}

void timeRange(const ErrEntry& e, char* buf, size_t len)
{
    char t0[ERRLOG_STAMP_LEN], t1[ERRLOG_STAMP_LEN];
    timeText(e.firstS, true, t0, sizeof(t0));
    if (e.count <= 1) { strlcpy(buf, t0, len); return; }
    // дата у последнего раза - если день другой или первый раз был до установки часов («+0:00:01»)
    const time_t a = Net::epochFromUptime(e.firstS), b = Net::epochFromUptime(e.lastS);
    bool withDate = b != 0 && a == 0;
    if (a && b) {
        struct tm ta, tb;
        localtime_r(&a, &ta);
        localtime_r(&b, &tb);
        withDate = ta.tm_yday != tb.tm_yday || ta.tm_year != tb.tm_year;
    }
    timeText(e.lastS, withDate, t1, sizeof(t1));
    snprintf(buf, len, "%s - %s x%u", t0, t1, e.count);
}

void format(const ErrEntry& e, char* buf, size_t len)
{
    char t[ERRLOG_TIME_LEN], what[ERRLOG_LINE_LEN];
    timeRange(e, t, sizeof(t));
    describe(e.dev, e.code, e.arg, what, sizeof(what));
    snprintf(buf, len, "%s  %s", t, what);
}

} // namespace ErrLog
