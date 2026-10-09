// s8.cpp - датчик CO2 SenseAir S8 LP 004-0-0053 (CO2_SENSOR == CO2_SENSOR_S8), UART Modbus RTU
// (Docs/Senseair S8 Modbus.md, Docs/Senseair S8 LP product specification.md).
// Раз в период опроса (настройка co2.co2PollS) - запрос «статус + CO2» (функция 0x04, IR1..IR4, адрес 0xFE «любой датчик»),
// ответ 13 байт: адрес, функция, число байт (8), IR1 MeterStatus, IR2, IR3, IR4 Space CO2, CRC (младший первым).
// Между опросами - ABC, запрошенное из веба: чтение HR32 (0x03, ответ 7 байт) и запись (0x06, ответ - эхо 8 байт).
// Неблокирующий автомат: байты ответа забираются из буфера UART в update(), ожидание - по millis().
// Уровни UART датчика 3.3 В - подключение к ESP32-S3 напрямую. Порт - Serial1 (UART1) через GPIO-matrix.
#include "config.h"
#if CO2_SENSOR == CO2_SENSOR_S8
#include "co2_sensor.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace Co2Sensor {

constexpr uint32_t MS_PER_S = 1000;

enum class State : uint8_t { Idle, WaitResponse };
enum class Op : uint8_t { None, Co2, ReadAbc, WriteAbc };

constexpr uint8_t  REQ_LEN       = 8;                           // адрес, функция, рег. Hi/Lo, кол-во (значение) Hi/Lo, CRC Lo/Hi
constexpr uint8_t  RESP_HDR_LEN  = 3;                           // адрес, функция, число байт
constexpr uint8_t  CRC_LEN       = 2;
constexpr uint8_t  CO2_DATA_LEN  = 2 * S8_REG_COUNT;            // 8 байт данных
constexpr uint8_t  CO2_RESP_LEN  = RESP_HDR_LEN + CO2_DATA_LEN + CRC_LEN;   // 13
constexpr uint8_t  ABC_DATA_LEN  = 2;                           // один регистр HR32
constexpr uint8_t  ABC_RESP_LEN  = RESP_HDR_LEN + ABC_DATA_LEN + CRC_LEN;   // 7
constexpr uint8_t  RESP_MAX      = CO2_RESP_LEN;
constexpr uint8_t  EXC_LEN       = 5;                           // адрес, функция|0x80, код, CRC
constexpr uint16_t CRC_INIT      = 0xFFFF;
constexpr uint16_t CRC_POLY      = 0xA001;                      // Modbus CRC-16 (0x8005, отражённый)
constexpr uint8_t  BITS_PER_BYTE = 8;

static HardwareSerial& s_uart = Serial1;
static State    s_state     = State::Idle;
static Op       s_op        = Op::None;
static uint8_t  s_req[REQ_LEN];
static uint8_t  s_respLen   = 0;            // ожидаемая длина ответа
static uint32_t s_stateTime = 0;
static uint32_t s_periodMs  = (uint32_t)CO2_POLL_DEF_S * MS_PER_S;   // период опроса, мс (из update)
static uint32_t s_lastPoll  = 0;
static uint32_t s_lastValid = 0;
static bool     s_everValid = false;
static uint16_t s_co2       = 0;
static uint16_t s_status    = 0;        // последний MeterStatus
static uint32_t s_errors    = 0;
static uint8_t  s_rx[RESP_MAX];
static uint8_t  s_rxLen     = 0;

// ABC
static AbcState s_abcState  = AbcState::None;
static Op       s_abcNext   = Op::None;
static uint16_t s_abcWrite  = 0;        // значение HR32 к записи (0 - выключить)
static uint16_t s_period    = 0;        // HR32, ч (0 - ABC приостановлен)
static uint16_t s_lastOnPeriod = 0;     // последний ненулевой период (для «включить» без нового периода)
static char     s_abcMsg[ABC_MSG_LEN] = "";

static uint16_t crc16(const uint8_t* d, size_t n)
{
    uint16_t crc = CRC_INIT;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < BITS_PER_BYTE; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ CRC_POLY) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << BITS_PER_BYTE) | p[1]); }

void begin()
{
    s_uart.begin(S8_BAUD, SERIAL_8N1, S8_PIN_RX, S8_PIN_TX);
    s_lastPoll = millis() - s_periodMs;   // первый опрос сразу
}

// Запрос: функция, регистр, кол-во регистров (0x03 / 0x04) или значение (0x06)
static void sendRequest(uint8_t func, uint16_t reg, uint16_t arg, uint8_t respLen)
{
    s_req[0] = S8_MODBUS_ADDR;
    s_req[1] = func;
    s_req[2] = (uint8_t)(reg >> BITS_PER_BYTE);
    s_req[3] = (uint8_t)reg;
    s_req[4] = (uint8_t)(arg >> BITS_PER_BYTE);
    s_req[5] = (uint8_t)arg;
    const uint16_t crc = crc16(s_req, REQ_LEN - CRC_LEN);
    s_req[REQ_LEN - 2] = (uint8_t)crc;          // CRC - младший байт первым
    s_req[REQ_LEN - 1] = (uint8_t)(crc >> BITS_PER_BYTE);
    while (s_uart.available()) s_uart.read();   // мусор прошлых обменов
    s_uart.write(s_req, REQ_LEN);
    s_respLen = respLen;
}

static bool crcOk(uint8_t len)
{
    const uint16_t crc = (uint16_t)(s_rx[len - 2] | ((uint16_t)s_rx[len - 1] << BITS_PER_BYTE));
    return crc16(s_rx, len - CRC_LEN) == crc;
}

// true - кадр корректный (CO2 и статус обновлены)
static bool parseCo2()
{
    if (s_rx[0] != S8_MODBUS_ADDR || s_rx[1] != S8_FUNC_READ_INPUT || s_rx[2] != CO2_DATA_LEN || !crcOk(CO2_RESP_LEN)) return false;
    s_status = be16(&s_rx[RESP_HDR_LEN + 2 * S8_REG_IDX_STATUS]);
    const int16_t v = (int16_t)be16(&s_rx[RESP_HDR_LEN + 2 * S8_REG_IDX_CO2]);
    s_co2 = v < 0 ? 0 : (uint16_t)v;
    return true;
}

static void abcFail(const char* why)
{
    snprintf(s_abcMsg, sizeof(s_abcMsg), "%s", why);
    s_abcState = AbcState::Error;
    s_abcNext  = Op::None;
}

// Ответ шага ABC: чтение HR32 - значение; запись - эхо запроса, затем перечитать
static void abcResponse()
{
    if (s_op == Op::ReadAbc) {
        if (s_rx[0] != S8_MODBUS_ADDR || s_rx[1] != S8_FUNC_READ_HOLDING || s_rx[2] != ABC_DATA_LEN || !crcOk(ABC_RESP_LEN)) {
            abcFail("чтение HR32: неверный ответ");
            return;
        }
        s_period = be16(&s_rx[RESP_HDR_LEN]);
        if (s_period) s_lastOnPeriod = s_period;
        s_abcMsg[0] = '\0';
        s_abcState  = AbcState::Ok;
        s_abcNext   = Op::None;
    } else {
        if (memcmp(s_rx, s_req, REQ_LEN) != 0) { abcFail("запись HR32: ответ не совпал с запросом"); return; }
        s_abcNext = Op::ReadAbc;
    }
}

static void startOp(Op op, uint32_t now)
{
    switch (op) {
    case Op::ReadAbc:  sendRequest(S8_FUNC_READ_HOLDING, S8_HR_ABC_PERIOD, 1, ABC_RESP_LEN); break;
    case Op::WriteAbc: sendRequest(S8_FUNC_WRITE_SINGLE, S8_HR_ABC_PERIOD, s_abcWrite, REQ_LEN); break;   // ответ - эхо
    default:           sendRequest(S8_FUNC_READ_INPUT, S8_REG_FIRST, S8_REG_COUNT, CO2_RESP_LEN); break;
    }
    s_op        = op;
    s_rxLen     = 0;
    s_stateTime = now;
    s_state     = State::WaitResponse;
}

void update(uint32_t now, uint8_t periodS)
{
    s_periodMs = (uint32_t)periodS * MS_PER_S;   // настройка меняется на лету
    switch (s_state) {
    case State::Idle:
        if (s_abcNext != Op::None) { startOp(s_abcNext, now); break; }   // ABC - между опросами CO2
        if (now - s_lastPoll < s_periodMs) break;
        s_lastPoll = now;
        startOp(Op::Co2, now);
        break;

    case State::WaitResponse:
        while (s_rxLen < s_respLen && s_uart.available()) s_rx[s_rxLen++] = (uint8_t)s_uart.read();
        if (s_rxLen >= EXC_LEN && s_rx[1] == (s_req[1] | S8_EXCEPTION_FLAG)) {   // исключение Modbus
            if (s_op == Op::Co2) s_errors++;
            else {
                char why[ABC_MSG_LEN];
                snprintf(why, sizeof(why), "датчик ответил исключением Modbus %u", s_rx[2]);
                abcFail(why);
            }
            s_state = State::Idle;
        } else if (s_rxLen == s_respLen) {
            if (s_op == Op::Co2) {
                if (!parseCo2())                             s_errors++;
                else if ((s_status & S8_STATUS_ERRORS) == 0) { s_lastValid = now; s_everValid = true; }
            } else {
                abcResponse();
            }
            s_state = State::Idle;                      // ошибка самого датчика - данные не валидны (см. statusText)
        } else if (now - s_stateTime > S8_RESPONSE_TIMEOUT_MS) {
            if (s_op == Op::Co2) s_errors++;            // нет ответа / неполный кадр
            else abcFail("нет ответа датчика");
            s_state = State::Idle;
        }
        break;
    }
}

bool     isValid(uint32_t now) { return s_everValid && (now - s_lastValid) < CO2_FAIL_PERIODS * s_periodMs; }
uint16_t co2()                 { return s_co2; }
uint32_t errorCount()          { return s_errors; }

void statusText(char* buf, size_t len)
{
    const char* state = isValid(millis()) ? "OK" : "нет данных";
    if (s_status & S8_STATUS_ERRORS)
        snprintf(buf, len, "%s, ошибка датчика 0x%02X, ошибок обмена: %lu", state, s_status, (unsigned long)s_errors);
    else
        snprintf(buf, len, "%s, %u ppm, статус 0x%02X, ошибок обмена: %lu", state, s_co2, s_status, (unsigned long)s_errors);
}

// ------------------------------------------------------------------ ABC
bool abcRequestRead()
{
    if (s_abcState == AbcState::Busy) return false;
    s_abcMsg[0] = '\0';
    s_abcState  = AbcState::Busy;
    s_abcNext   = Op::ReadAbc;
    return true;
}

// Вкл/выкл и период - один регистр: выкл - 0, вкл - период. Чего нет в запросе - из последнего чтения.
bool abcRequestWrite(int32_t periodH, int8_t on)
{
    if (s_abcState == AbcState::Busy || s_abcState == AbcState::None) return false;   // сначала прочитать
    if (periodH < 0 && on < 0) return false;
    if (periodH >= 0 && (periodH < ABC_PERIOD_MIN_H || periodH > ABC_PERIOD_MAX_H)) return false;
    const bool     wantOn = on < 0 ? s_period != 0 : on != 0;
    const uint16_t period = periodH >= 0 ? (uint16_t)periodH : s_lastOnPeriod;
    if (wantOn && period == 0) return false;   // включить, а период неизвестен
    s_abcWrite  = wantOn ? period : 0;
    s_abcMsg[0] = '\0';
    s_abcState  = AbcState::Busy;
    s_abcNext   = Op::WriteAbc;
    return true;
}

AbcState    abcState()   { return s_abcState; }
uint16_t    abcPeriodH() { return s_period; }
bool        abcOn()      { return s_period != 0; }
const char* abcMessage() { return s_abcMsg; }

} // namespace Co2Sensor
#endif // CO2_SENSOR_S8
