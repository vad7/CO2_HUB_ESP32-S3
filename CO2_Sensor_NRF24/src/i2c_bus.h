#pragma once
// Частота общей шины I2C (K22, SHT40, тач FT6336G на ES3C28P) - настройка co2.i2cKhz.
// main.cpp передаёт её из настроек каждый проход loop(); драйверы берут I2cBus::hz() на каждую транзакцию
// (lgfx::i2c задаёт частоту в транзакции - смена действует со следующего обмена).
#include <stdint.h>

namespace I2cBus {
    void     setKhz(uint16_t khz);   // вне I2C_KHZ_MIN..MAX - ограничивается
    uint32_t hz();                   // текущая частота, Гц
}
