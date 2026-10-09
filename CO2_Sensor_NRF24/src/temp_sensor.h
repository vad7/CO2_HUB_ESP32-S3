// temp_sensor.h - датчик температуры (необязательный), тип выбирается в настройках (Cfg::co2.tempSensor):
// нет / DS18B20 (1-Wire, ds18b20.cpp) / SHT40 (I2C, sht40.cpp). Смена типа - на лету (из меню или веба).
// Неблокирующий: update() из loop(), за вызов - один короткий шаг обмена (≤ ~0.6 мс), ожидание
// преобразования (DS18B20 - 750 мс, SHT40 - 10 мс) - по millis().
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace TempSensor {
    void        update(uint32_t now, uint8_t type, uint8_t periodS);   // type - TempSensorType, periodS - период, с
    bool        active();                        // датчик выбран
    bool        valid();                         // есть значение и меньше TEMP_FAIL_COUNT ошибок подряд
    int16_t     tenthsC();                       // последняя температура, 0.1 °C
    int8_t      humidity();                      // последняя влажность, % (-1 - датчик её не меряет: DS18B20)
    const char* typeName(uint8_t type);          // «нет» / «DS18B20» / «SHT40»
    void        statusText(char* buf, size_t len);   // для веба: «23.4 °C, 45 % (SHT40, I2C 0x44, ошибок 0)» / «нет ответа ...»
}
