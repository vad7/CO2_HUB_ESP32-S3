// temp_sensor.h - датчик температуры (необязательный), тип выбирается в настройках (Cfg::co2.tempSensor):
// нет / DS18B20 (1-Wire, ds18b20.cpp) / SHT40 (I2C, sht40.cpp). Смена типа - на лету (из меню или веба).
// Неблокирующий: update() из loop(), за вызов - один короткий шаг обмена (≤ ~0.6 мс), ожидание
// преобразования (DS18B20 - 750 мс, SHT40 - 10 мс) - по millis().
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "temp_backend.h"   // TempErr

namespace TempSensor {
    // type - TempSensorType, periodS - период, с; неудачное чтение - до retries повторов через retryMs
    void        update(uint32_t now, uint8_t type, uint8_t periodS, uint8_t retries, uint16_t retryMs);
    bool        active();                        // датчик выбран
    bool        valid();                         // есть значение и меньше TEMP_FAIL_COUNT ошибок подряд
    int16_t     tenthsC();                       // последняя температура, 0.1 °C
    int8_t      humidity();                      // последняя влажность, % округлённо (-1 - датчик её не меряет: DS18B20)
    int16_t     humidityTenths();                // то же, 0.1 % (SHT40 - разрешение 0.01 %); выводится целым (humidity)
    const char* typeName(uint8_t type);          // «нет» / «DS18B20» / «SHT40»
    void        statusText(char* buf, size_t len);   // для веба: «23.4 °C, 45 % (SHT40, I2C 0x44, ошибок 0)» / «нет ответа ...»
    const char* errText(TempErr e, bool brief = false);   // описание ошибки (brief - коротко, для шапки экрана)
    TempErr     lastError();
    uint8_t     failRun();                       // неудачных чтений подряд (0 - последнее удачное)
    void        serialText(char* buf, size_t len);   // серийный номер: DS18B20 «28-0123456789AB», SHT40 «0x12345678»; пусто - нет
    uint8_t     resolutionBits();                // DS18B20: разрешение, бит (0 - не DS18B20 или ещё не читали)
    bool        requestResolution(uint8_t bits); // DS18B20: записать разрешение в датчик; false - не DS18B20 / неверно
}
