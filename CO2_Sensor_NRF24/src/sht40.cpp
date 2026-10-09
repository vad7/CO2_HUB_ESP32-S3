// sht40.cpp - драйвер Sensirion SHT40 (SHT4x) по I2C: температура и влажность.
// Шина - та же, что у K22 / тача (I2C_PORT, драйвер LovyanGFX lgfx::i2c, K22_I2C_HZ = 100 кГц).
// Адрес: 0x44 (SHT40-AD1B), варианты 0x45 / 0x46 - ищутся по ACK при первом измерении (SHT40_ADDRS).
// Измерение (даташит SHT4x): команда 0xFD (высокая точность) -> ожидание до 8.2 мс (SHT40_MEASURE_MS) ->
// 6 байт: T MSB, T LSB, CRC, RH MSB, RH LSB, CRC. CRC-8: полином 0x31, начальное 0xFF, без отражения.
// T = -45 + 175 * S_T / 65535 °C;  RH = -6 + 125 * S_RH / 65535 %, ограничить 0..100.
// Неблокирующий: запись команды (~0.3 мс) и чтение (~0.7 мс) - разные вызовы poll(), ожидание - по millis().
#include "temp_backend.h"
#include "config.h"
#include "lgfx_include.h"

namespace Sht40 {

constexpr uint8_t  RESP_LEN      = 6;
constexpr uint8_t  CRC_INIT      = 0xFF;
constexpr uint8_t  CRC_POLY      = 0x31;
constexpr uint8_t  BITS_PER_BYTE = 8;
constexpr uint8_t  MSB_BIT       = 0x80;
constexpr int32_t  RAW_FULL      = 65535;
constexpr int32_t  T_OFFSET_10   = -450;    // -45.0 °C, в 0.1 °C
constexpr int32_t  T_SPAN_10     = 1750;    // 175.0 °C
constexpr int32_t  RH_OFFSET     = -6;      // %
constexpr int32_t  RH_SPAN       = 125;
constexpr int32_t  RH_MAX        = 100;
constexpr uint8_t  ADDR_COUNT    = sizeof(SHT40_ADDRS) / sizeof(SHT40_ADDRS[0]);

enum class State : uint8_t { Command, Measuring };

static State    s_state = State::Command;
static uint8_t  s_addr  = 0;            // найденный адрес (0 - ещё не найден)
static uint32_t s_cmdTime = 0;

static bool sendCommand(uint8_t addr)
{
    const uint8_t cmd = SHT40_CMD_MEASURE_HI;
    return lgfx::i2c::transactionWrite(I2C_PORT, addr, &cmd, 1, K22_I2C_HZ).has_value();
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

void attach()
{
    lgfx::i2c::init(I2C_PORT, I2C_PIN_SDA, I2C_PIN_SCL);   // на ES3C28P / с K22 - уже инициализирована
    s_addr = 0;
}

void detach() {}   // шина общая - остаётся K22 и тачу

void start(uint32_t) { s_state = State::Command; }

uint8_t address() { return s_addr; }

TempPoll poll(uint32_t now, TempErr& err, int16_t& tenths, int8_t& rh)
{
    rh = -1;
    if (s_state == State::Command) {
        bool ok = s_addr != 0 && sendCommand(s_addr);
        for (uint8_t i = 0; !ok && i < ADDR_COUNT; i++) {   // адрес неизвестен или датчик пропал - поиск
            ok = sendCommand(SHT40_ADDRS[i]);
            if (ok) s_addr = SHT40_ADDRS[i];
        }
        if (!ok) { s_addr = 0; err = TempErr::NoAck; return TempPoll::Done; }
        s_cmdTime = now;
        s_state = State::Measuring;
        return TempPoll::Busy;
    }
    if (now - s_cmdTime < SHT40_MEASURE_MS) return TempPoll::Busy;
    s_state = State::Command;
    uint8_t r[RESP_LEN] = {0};
    if (!lgfx::i2c::transactionRead(I2C_PORT, s_addr, r, RESP_LEN, K22_I2C_HZ).has_value()) { err = TempErr::NoAck; return TempPoll::Done; }
    if (crc8(r, 2) != r[2] || crc8(r + 3, 2) != r[5]) { err = TempErr::Crc; return TempPoll::Done; }
    const int32_t st  = ((int32_t)r[0] << BITS_PER_BYTE) | r[1];
    const int32_t srh = ((int32_t)r[3] << BITS_PER_BYTE) | r[4];
    tenths = (int16_t)(T_OFFSET_10 + (T_SPAN_10 * st + RAW_FULL / 2) / RAW_FULL);
    int32_t h = RH_OFFSET + (RH_SPAN * srh + RAW_FULL / 2) / RAW_FULL;
    rh = (int8_t)(h < 0 ? 0 : (h > RH_MAX ? RH_MAX : h));
    err = TempErr::None;
    return TempPoll::Done;
}

} // namespace Sht40
