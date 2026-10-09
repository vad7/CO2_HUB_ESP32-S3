// co2_sensor.h - датчик CO2: общий интерфейс. Реализация выбирается CO2_SENSOR в config.h:
//   CO2_SENSOR_K22 - k22.cpp (SenseAir K22-OC, I2C), CO2_SENSOR_S8 - s8.cpp (SenseAir S8 LP, UART Modbus).
// Обе - неблокирующие автоматы (запрос -> ожидание ответа), update() из loop().
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Co2Sensor {
    void     begin();
    void     update(uint32_t now, uint8_t periodS); // вызывать из loop(); periodS - период опроса (co2.co2PollS)
    bool     isValid(uint32_t now);                 // были валидные данные за CO2_FAIL_PERIODS периодов опроса
    uint16_t co2();                                 // последнее значение, ppm (отрицательные -> 0)
    uint32_t errorCount();                          // ошибок обмена
    void     statusText(char* buf, size_t len);     // состояние для веба: «OK, 812 ppm, ...»

    // ABC (автоматическая коррекция базы) - чтение и запись в датчике. Асинхронно: запрос (из веба, под CfgLock)
    // ставит задачу, её выполняет update() между опросами CO2; итог - abcState(). После записи - перечитывается.
    //   K22: период - EEPROM 0x40, вкл/выкл - бит 1 MeterControl (EEPROM 0x3E; действует после перезапуска питания).
    //   S8:  период - HR32; выкл = период 0 (вкл - записать период ≠ 0).
    enum class AbcState : uint8_t { None, Busy, Ok, Error };   // None - ещё не читали
    bool        abcRequestRead();                   // false - занят предыдущей задачей
    bool        abcRequestWrite(int32_t periodH, int8_t on);   // -1 - не менять; false - занят / не читали / неверно
    AbcState    abcState();
    uint16_t    abcPeriodH();                       // период, ч (S8 при выключенном ABC - 0)
    bool        abcOn();
    const char* abcMessage();                       // ошибка или примечание к результату (может быть пустой)
}
