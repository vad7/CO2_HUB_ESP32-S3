// Частота общей шины I2C - см. i2c_bus.h
#include "i2c_bus.h"
#include "config.h"

namespace I2cBus {

static uint32_t s_hz = (uint32_t)I2C_KHZ_DEF * HZ_PER_KHZ;   // текущая частота шины, Гц

void setKhz(uint16_t khz)
{
    if (khz < I2C_KHZ_MIN) khz = I2C_KHZ_MIN;
    if (khz > I2C_KHZ_MAX) khz = I2C_KHZ_MAX;
    s_hz = (uint32_t)khz * HZ_PER_KHZ;
}

uint32_t hz() { return s_hz; }

} // namespace I2cBus
