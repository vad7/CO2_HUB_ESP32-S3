// k22.cpp — датчик CO2 SenseAir K22-OC (CO2_SENSOR == CO2_SENSOR_K22): чтение CO2 из RAM 0x08..0x09
// по I2C (Docs/SensAir_I2C_comm_guide_2_1031.pdf, прил. B). Неблокирующий автомат: запрос -> пауза tWAIT -> чтение.
// I2C через драйвер LovyanGFX: на ES3C28P шина I2C0 общая с тачем FT6336G,
// один драйвер на пинах исключает конфликт с Arduino Wire.
#include "config.h"
#if CO2_SENSOR == CO2_SENSOR_K22
#include "co2_sensor.h"
#include <stdio.h>
#include "lgfx_include.h"

namespace Co2Sensor {

enum class State : uint8_t { Idle, WaitResponse };

static State    s_state       = State::Idle;
static uint32_t s_stateTime   = 0;
static uint32_t s_lastPoll    = 0;
static uint32_t s_lastValid   = 0;
static bool     s_everValid   = false;
static uint16_t s_co2         = 0;
static uint8_t  s_retries     = 0;
static uint32_t s_errors      = 0;

// Запрос: команда, адрес RAM (2 байта), контрольная сумма = сумма байт без адреса I2C
static const uint8_t REQUEST[] = {
    K22_CMD_READ_RAM_2, K22_RAM_CO2_HI, K22_RAM_CO2_LO,
    (uint8_t)(K22_CMD_READ_RAM_2 + K22_RAM_CO2_HI + K22_RAM_CO2_LO)   // 0x2A
};
enum : uint8_t { RESP_STATUS = 0, RESP_HI, RESP_LO, RESP_CSUM, RESP_LEN };
constexpr uint8_t STATUS_COMPLETE = 0x01;

void begin()
{
    lgfx::i2c::init(I2C_PORT, I2C_PIN_SDA, I2C_PIN_SCL);
    s_lastPoll = millis() - K22_POLL_PERIOD_MS;   // первый опрос сразу
}

static bool sendRequest()
{
    return lgfx::i2c::transactionWrite(I2C_PORT, K22_I2C_ADDR, REQUEST, sizeof(REQUEST), K22_I2C_HZ).has_value();
}

// 1 — данные приняты, 0 — датчик не готов (повторить), -1 — ошибка
static int8_t readResponse()
{
    uint8_t r[RESP_LEN] = {0};
    if (!lgfx::i2c::transactionRead(I2C_PORT, K22_I2C_ADDR, r, RESP_LEN, K22_I2C_HZ).has_value()) return 0; // NACK = занят
    if (!(r[RESP_STATUS] & STATUS_COMPLETE)) return 0;
    if ((uint8_t)(r[RESP_STATUS] + r[RESP_HI] + r[RESP_LO]) != r[RESP_CSUM]) return -1;
    int16_t v = (int16_t)(((uint16_t)r[RESP_HI] << 8) | r[RESP_LO]);
    s_co2 = v < 0 ? 0 : (uint16_t)v;   // отрицательные бывают при тесте азотом
    return 1;
}

void update(uint32_t now)
{
    switch (s_state) {
    case State::Idle:
        if (now - s_lastPoll < K22_POLL_PERIOD_MS) break;
        s_lastPoll = now;
        if (sendRequest()) {
            s_retries   = 0;
            s_stateTime = now;
            s_state     = State::WaitResponse;
        } else {
            s_errors++;
        }
        break;

    case State::WaitResponse:
        if (now - s_stateTime < K22_WAIT_MS) break;
        switch (readResponse()) {
        case 1:
            s_lastValid = now;
            s_everValid = true;
            s_state = State::Idle;
            break;
        case 0:
            if (++s_retries < K22_READ_RETRIES) { s_stateTime = now; break; }
            [[fallthrough]];   // исчерпаны повторы
        default:
            s_errors++;
            s_state = State::Idle;
            break;
        }
        break;
    }
}

bool     isValid(uint32_t now) { return s_everValid && (now - s_lastValid) < K22_FAIL_TIMEOUT_MS; }
uint16_t co2()                 { return s_co2; }
uint32_t errorCount()          { return s_errors; }

void statusText(char* buf, size_t len)
{
    if (isValid(millis())) snprintf(buf, len, "OK, %u ppm, ошибок обмена: %lu", s_co2, (unsigned long)s_errors);
    else                   snprintf(buf, len, "нет данных, ошибок обмена: %lu", (unsigned long)s_errors);
}

} // namespace Co2Sensor
#endif // CO2_SENSOR_K22
