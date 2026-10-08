// s8.cpp — датчик CO2 SenseAir S8 LP 004-0-0053 (CO2_SENSOR == CO2_SENSOR_S8), UART Modbus RTU
// (Docs/Senseair S8 Modbus.md, Docs/Senseair S8 LP product specification.md).
// Раз в S8_POLL_PERIOD_MS — запрос «статус + CO2» (функция 0x04, IR1..IR4, адрес 0xFE «любой датчик»),
// ответ 13 байт: адрес, функция, число байт (8), IR1 MeterStatus, IR2, IR3, IR4 Space CO2, CRC (младший первым).
// Неблокирующий автомат: байты ответа забираются из буфера UART в update(), ожидание — по millis().
// Уровни UART датчика 3.3 В — подключение к ESP32-S3 напрямую. Порт — Serial1 (UART1) через GPIO-matrix.
#include "config.h"
#if CO2_SENSOR == CO2_SENSOR_S8
#include "co2_sensor.h"
#include <Arduino.h>
#include <stdio.h>

namespace Co2Sensor {

constexpr uint8_t  REQ_LEN       = 8;                           // адрес, функция, рег. Hi/Lo, кол-во Hi/Lo, CRC Lo/Hi
constexpr uint8_t  RESP_DATA_LEN = 2 * S8_REG_COUNT;            // 8 байт данных
constexpr uint8_t  RESP_HDR_LEN  = 3;                           // адрес, функция, число байт
constexpr uint8_t  RESP_LEN      = RESP_HDR_LEN + RESP_DATA_LEN + 2;   // + CRC = 13
constexpr uint8_t  EXC_LEN       = 5;                           // адрес, функция|0x80, код, CRC
constexpr uint16_t CRC_INIT      = 0xFFFF;
constexpr uint16_t CRC_POLY      = 0xA001;                      // Modbus CRC-16 (0x8005, отражённый)
constexpr uint8_t  BITS_PER_BYTE = 8;

enum class State : uint8_t { Idle, WaitResponse };

static HardwareSerial& s_uart = Serial1;
static State    s_state     = State::Idle;
static uint32_t s_stateTime = 0;
static uint32_t s_lastPoll  = 0;
static uint32_t s_lastValid = 0;
static bool     s_everValid = false;
static uint16_t s_co2       = 0;
static uint16_t s_status    = 0;        // последний MeterStatus
static uint32_t s_errors    = 0;
static uint8_t  s_rx[RESP_LEN];
static uint8_t  s_rxLen     = 0;

static uint16_t crc16(const uint8_t* d, size_t n)
{
    uint16_t crc = CRC_INIT;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < BITS_PER_BYTE; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ CRC_POLY) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

void begin()
{
    s_uart.begin(S8_BAUD, SERIAL_8N1, S8_PIN_RX, S8_PIN_TX);
    s_lastPoll = millis() - S8_POLL_PERIOD_MS;   // первый опрос сразу
}

static void sendRequest()
{
    uint8_t req[REQ_LEN] = { S8_MODBUS_ADDR, S8_FUNC_READ_INPUT,
                             (uint8_t)(S8_REG_FIRST >> 8), (uint8_t)S8_REG_FIRST,
                             (uint8_t)(S8_REG_COUNT >> 8), (uint8_t)S8_REG_COUNT, 0, 0 };
    const uint16_t crc = crc16(req, REQ_LEN - 2);
    req[REQ_LEN - 2] = (uint8_t)crc;          // CRC — младший байт первым
    req[REQ_LEN - 1] = (uint8_t)(crc >> 8);
    while (s_uart.available()) s_uart.read(); // мусор прошлых обменов
    s_uart.write(req, REQ_LEN);
}

// true — кадр корректный (CO2 и статус обновлены)
static bool parseResponse()
{
    if (s_rx[0] != S8_MODBUS_ADDR || s_rx[1] != S8_FUNC_READ_INPUT || s_rx[2] != RESP_DATA_LEN) return false;
    const uint16_t crc = (uint16_t)(s_rx[RESP_LEN - 2] | ((uint16_t)s_rx[RESP_LEN - 1] << 8));
    if (crc16(s_rx, RESP_LEN - 2) != crc) return false;
    s_status = be16(&s_rx[RESP_HDR_LEN + 2 * S8_REG_IDX_STATUS]);
    const int16_t v = (int16_t)be16(&s_rx[RESP_HDR_LEN + 2 * S8_REG_IDX_CO2]);
    s_co2 = v < 0 ? 0 : (uint16_t)v;
    return true;
}

void update(uint32_t now)
{
    switch (s_state) {
    case State::Idle:
        if (now - s_lastPoll < S8_POLL_PERIOD_MS) break;
        s_lastPoll = now;
        sendRequest();
        s_rxLen     = 0;
        s_stateTime = now;
        s_state     = State::WaitResponse;
        break;

    case State::WaitResponse:
        while (s_rxLen < RESP_LEN && s_uart.available()) s_rx[s_rxLen++] = (uint8_t)s_uart.read();
        if (s_rxLen >= EXC_LEN && s_rx[1] == (S8_FUNC_READ_INPUT | S8_EXCEPTION_FLAG)) {   // исключение Modbus
            s_errors++;
            s_state = State::Idle;
        } else if (s_rxLen == RESP_LEN) {
            if (!parseResponse())                       s_errors++;
            else if ((s_status & S8_STATUS_ERRORS) == 0) { s_lastValid = now; s_everValid = true; }
            s_state = State::Idle;                      // ошибка самого датчика — данные не валидны (см. statusText)
        } else if (now - s_stateTime > S8_RESPONSE_TIMEOUT_MS) {
            s_errors++;                                 // нет ответа / неполный кадр
            s_state = State::Idle;
        }
        break;
    }
}

bool     isValid(uint32_t now) { return s_everValid && (now - s_lastValid) < S8_FAIL_TIMEOUT_MS; }
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

} // namespace Co2Sensor
#endif // CO2_SENSOR_S8
