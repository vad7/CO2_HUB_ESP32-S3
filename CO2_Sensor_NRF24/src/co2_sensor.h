// co2_sensor.h — датчик CO2: общий интерфейс. Реализация выбирается CO2_SENSOR в config.h:
//   CO2_SENSOR_K22 — k22.cpp (SenseAir K22-OC, I2C), CO2_SENSOR_S8 — s8.cpp (SenseAir S8 LP, UART Modbus).
// Обе — неблокирующие автоматы (запрос -> ожидание ответа), update() из loop().
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Co2Sensor {
    void     begin();
    void     update(uint32_t now);                  // вызывать из loop()
    bool     isValid(uint32_t now);                 // были валидные данные за *_FAIL_TIMEOUT_MS
    uint16_t co2();                                 // последнее значение, ppm (отрицательные -> 0)
    uint32_t errorCount();                          // ошибок обмена
    void     statusText(char* buf, size_t len);     // состояние для веба: «OK, 812 ppm, ...»
}
