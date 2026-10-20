// sht40.cpp - драйвер Sensirion SHT40 (SHT4x) по I2C: температура и влажность (Docs/Sensirion_Datasheet_SHT4x.pdf).
// Шина - та же, что у K22 / тача (I2C_PORT, драйвер LovyanGFX lgfx::i2c, частота - I2cBus::hz(), настройка co2.i2cKhz).
// Адрес: 0x44 (SHT40-AD1B), варианты 0x45 / 0x46 - ищутся по ACK при первом измерении (SHT40_ADDRS).
// Измерение: команда 0xFD (высокая точность, табл. 8) -> ожидание до 8.3 мс (SHT40_MEASURE_MS, табл. 5) ->
// 6 байт: T MSB, T LSB, CRC, RH MSB, RH LSB, CRC. CRC-8: полином 0x31, начальное 0xFF, без отражения.
// T = -45 + 175 * S_T / 65535 °C;  RH = -6 + 125 * S_RH / 65535 %, ограничить 0..100 (гл. 4.6).
// Серийный номер (гл. 4.7): команда 0x89 -> 2 слова по 16 бит, за каждым CRC; читается в цикле после найденного адреса,
// не готов - NACK на чтение (табл. 8): повтор, не больше SHT40_SERIAL_TRIES раз.
// Неблокирующий: запись команды (~0.3 мс) и чтение (~0.7 мс) - разные вызовы poll(), ожидание - по millis().
#include "temp_backend.h"
#include "config.h"
#include "lgfx_include.h"
#include "i2c_bus.h"

namespace Sht40 {

constexpr uint8_t  RESP_LEN      = 6;
constexpr uint8_t  CRC_INIT      = 0xFF;
constexpr uint8_t  CRC_POLY      = 0x31;
constexpr uint8_t  BITS_PER_BYTE = 8;
constexpr uint8_t  BITS_PER_WORD = 16;
constexpr uint8_t  MSB_BIT       = 0x80;
constexpr int32_t  RAW_FULL      = 65535;
constexpr int32_t  T_OFFSET_10   = -450;    // -45.0 °C, в 0.1 °C
constexpr int32_t  T_SPAN_10     = 1750;    // 175.0 °C
constexpr int32_t  RH_OFFSET_10  = -60;     // -6.0 %, в 0.1 %
constexpr int32_t  RH_SPAN_10    = 1250;    // 125.0 %
constexpr int32_t  RH_MAX_10     = 1000;    // 100.0 %
constexpr uint8_t  ADDR_COUNT    = sizeof(SHT40_ADDRS) / sizeof(SHT40_ADDRS[0]);

enum class State : uint8_t { SerialCmd, SerialRead, Command, Measuring };

static State    s_state   = State::Command;         // шаг автомата
static uint8_t  s_addr    = 0;            // найденный адрес (0 - ещё не найден)
static uint32_t s_cmdTime = 0;                      // момент команды (millis)
static uint32_t s_serial  = 0;                      // серийный номер
static bool     s_snOk    = false;                  // серийный номер прочитан
static uint8_t  s_snTries = 0;                      // попыток прочитать серийный номер

static bool sendCommand(uint8_t addr, uint8_t cmd)
{
    return lgfx::i2c::transactionWrite(I2C_PORT, addr, &cmd, 1, I2cBus::hz()).has_value();
}

static uint8_t crc8(const uint8_t* d, size_t n)
{
    uint8_t crc = CRC_INIT;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < BITS_PER_BYTE; b++) crc = (crc & MSB_BIT) ? (uint8_t)((crc << 1) ^ CRC_POLY) : (uint8_t)(crc << 1);
    }
    return crc;
}

static bool readResponse(uint8_t* r)
{
    if (!lgfx::i2c::transactionRead(I2C_PORT, s_addr, r, RESP_LEN, I2cBus::hz()).has_value()) return false;
    return true;
}

static bool crcOk(const uint8_t* r) { return crc8(r, 2) == r[2] && crc8(r + 3, 2) == r[5]; }

void attach()
{
    lgfx::i2c::init(I2C_PORT, I2C_PIN_SDA, I2C_PIN_SCL);   // на ES3C28P / с K22 - уже инициализирована
    s_addr = 0;
    s_snOk = false;
    s_snTries = 0;
}

void detach() {}   // шина общая - остаётся K22 и тачу

void start(uint32_t)
{
    s_state = (s_addr && !s_snOk && s_snTries < SHT40_SERIAL_TRIES) ? State::SerialCmd : State::Command;
}

uint8_t address() { return s_addr; }

bool serial(uint32_t& sn)
{
    sn = s_serial;
    return s_snOk;
}

TempPoll poll(uint32_t now, TempErr& err, int16_t& tenths, int16_t& rhTenths)
{
    rhTenths = -1;
    uint8_t r[RESP_LEN] = {0};
    switch (s_state) {
    case State::SerialCmd:                             // серийный номер - перед измерением, пока не прочитан
        s_snTries++;
        s_state = sendCommand(s_addr, SHT40_CMD_SERIAL) ? State::SerialRead : State::Command;
        s_cmdTime = now;
        return TempPoll::Busy;
    case State::SerialRead:
        if (now - s_cmdTime < SHT40_SERIAL_WAIT_MS) return TempPoll::Busy;
        if (readResponse(r)) {
            if (crcOk(r)) {
                s_serial = ((uint32_t)r[0] << (BITS_PER_WORD + BITS_PER_BYTE)) | ((uint32_t)r[1] << BITS_PER_WORD) |
                           ((uint32_t)r[3] << BITS_PER_BYTE) | r[4];
                s_snOk = true;
            }
            s_state = State::Command;
        } else if (s_snTries++ >= SHT40_SERIAL_TRIES) {
            s_state = State::Command;                  // не ответил - без номера, измерение идёт как обычно
        } else {
            s_cmdTime = now;                           // NACK - ещё не готов
        }
        return TempPoll::Busy;
    case State::Command: {
        bool ok = s_addr != 0 && sendCommand(s_addr, SHT40_CMD_MEASURE_HI);
        for (uint8_t i = 0; !ok && i < ADDR_COUNT; i++) {   // адрес неизвестен или датчик пропал - поиск
            ok = sendCommand(SHT40_ADDRS[i], SHT40_CMD_MEASURE_HI);
            if (ok) { s_addr = SHT40_ADDRS[i]; s_snOk = false; s_snTries = 0; }
        }
        if (!ok) { s_addr = 0; err = TempErr::NoAck; return TempPoll::Done; }
        s_cmdTime = now;
        s_state = State::Measuring;
        return TempPoll::Busy;
    }
    case State::Measuring:
        break;
    }
    if (now - s_cmdTime < SHT40_MEASURE_MS) return TempPoll::Busy;
    s_state = State::Command;
    if (!readResponse(r)) { err = TempErr::NoAck; return TempPoll::Done; }
    if (!crcOk(r))        { err = TempErr::Crc; return TempPoll::Done; }
    const int32_t st  = ((int32_t)r[0] << BITS_PER_BYTE) | r[1];
    const int32_t srh = ((int32_t)r[3] << BITS_PER_BYTE) | r[4];
    tenths = (int16_t)(T_OFFSET_10 + (T_SPAN_10 * st + RAW_FULL / 2) / RAW_FULL);
    const int32_t h = RH_OFFSET_10 + (RH_SPAN_10 * srh + RAW_FULL / 2) / RAW_FULL;
    rhTenths = (int16_t)(h < 0 ? 0 : (h > RH_MAX_10 ? RH_MAX_10 : h));
    err = TempErr::None;
    return TempPoll::Done;
}

} // namespace Sht40
