#pragma once
// Частота общей шины I2C (K22, SHT40, тач FT6336G на ES3C28P) - настройка co2.i2cKhz.
// main.cpp передаёт её из настроек каждый проход loop(); драйверы берут I2cBus::hz() на каждую транзакцию
// (lgfx::i2c задаёт частоту в транзакции - смена действует со следующего обмена).
// Не всякая частота годится: LovyanGFX на ESP32-S3 портит задержки START / STOP на части частот (config.h,
// I2C_SRC_HZ) - такие заменяются ближайшей допустимой (nearestKhz).
#include <stdint.h>

namespace I2cBus {
    void     setKhz(uint16_t khz);       // вне I2C_KHZ_MIN..MAX или недопустимая - ближайшая допустимая
    uint32_t hz();                       // текущая частота, Гц
    bool     khzOk(uint16_t khz);        // частота в пределах и LovyanGFX задаст её тайминги без искажений
    uint16_t nearestKhz(uint16_t khz);   // ближайшая допустимая (при равном расстоянии - меньшая)
}
