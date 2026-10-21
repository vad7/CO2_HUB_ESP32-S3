// Частота общей шины I2C - см. i2c_bus.h
#include "i2c_bus.h"
#include "config.h"

namespace I2cBus {

static uint32_t s_hz = (uint32_t)I2C_KHZ_DEF * HZ_PER_KHZ;   // текущая частота шины, Гц

static uint32_t ticksToNs(uint32_t t) { return (uint32_t)((uint64_t)t * NS_PER_S / I2C_SRC_HZ); }

// Повтор расчёта LovyanGFX (config.h, I2C_SRC_HZ): периоды SCL помещаются в поля, задержки START / STOP
// после усечения до 9 бит: до 100 кГц - не короче табл. 5 K22, выше - не усечены (как задумано библиотекой).
bool khzOk(uint16_t khz)
{
    if (khz < I2C_KHZ_MIN || khz > I2C_KHZ_MAX) return false;
    const uint32_t f = (uint32_t)khz * HZ_PER_KHZ;
    uint32_t cycle = I2C_SRC_HZ / (f + 1) + 1;
    if (cycle < I2C_LGFX_CYCLE_MIN) cycle = I2C_LGFX_CYCLE_MIN;
    const uint32_t total = cycle - I2C_LGFX_HIGH_OFFSET;
    uint32_t high = (total + 1) >> 1;
    if (high < I2C_LGFX_HIGH_MIN) high = I2C_LGFX_HIGH_MIN;
    const uint32_t low = total - high, wait = high >> I2C_LGFX_WAIT_SHIFT;
    if (high - wait > I2C_REG_MAX || wait > I2C_REG_WAIT_MAX || low > I2C_REG_MAX) return false;
    const uint32_t c = cycle < I2C_LGFX_CYCLE_CLAMP ? cycle : I2C_LGFX_CYCLE_CLAMP;
    const uint32_t start = c & I2C_REG_MAX, stop = (c << 1) & I2C_REG_MAX;   // что останется в 9-битных полях
    if (khz > I2C_STD_MAX_KHZ) return start == c && stop == (c << 1);
    return ticksToNs(start) >= I2C_T_START_NS && ticksToNs(stop) >= I2C_T_STOP_NS;
}

uint16_t nearestKhz(uint16_t khz)
{
    if (khz < I2C_KHZ_MIN) khz = I2C_KHZ_MIN;
    if (khz > I2C_KHZ_MAX) khz = I2C_KHZ_MAX;
    for (uint16_t d = 0; d <= I2C_KHZ_MAX - I2C_KHZ_MIN; d++) {
        if (khz >= I2C_KHZ_MIN + d && khzOk((uint16_t)(khz - d))) return (uint16_t)(khz - d);
        if (khz + d <= I2C_KHZ_MAX && khzOk((uint16_t)(khz + d))) return (uint16_t)(khz + d);
    }
    return I2C_KHZ_DEF;
}

void setKhz(uint16_t khz)
{
    static uint16_t s_lastKhz = 0;               // последняя запрошенная частота, кГц (вызов - каждый проход loop())
    if (khz == s_lastKhz) return;
    s_lastKhz = khz;
    if (!khzOk(khz)) khz = nearestKhz(khz);
    s_hz = (uint32_t)khz * HZ_PER_KHZ;
}

uint32_t hz() { return s_hz; }

} // namespace I2cBus
