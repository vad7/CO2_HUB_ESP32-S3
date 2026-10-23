// co2_sensor.h - датчик CO2: общий интерфейс. Реализация выбирается CO2_SENSOR в config.h:
//   CO2_SENSOR_K22 - k22.cpp (SenseAir K22-OC, I2C), CO2_SENSOR_S8 - s8.cpp (SenseAir S8 LP, UART Modbus).
// Обе - неблокирующие автоматы (запрос -> ожидание ответа), update() из loop().
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "errlog.h"   // Co2Err
#include "config.h"   // CO2_SENSOR

namespace Co2Sensor {
    void     begin();
    // вызывать из loop(); periodS - период опроса (co2.co2PollS); неудачный опрос повторяется до retries раз через
    // retryMs (co2.sensorRetries / co2.sensorRetryMs), в журнал - только неудача после всех повторов
    void     update(uint32_t now, uint8_t periodS, uint8_t retries, uint16_t retryMs);
    bool     isValid(uint32_t now);                 // были валидные данные за CO2_FAIL_PERIODS периодов опроса
    uint16_t co2();                                 // последнее значение, ppm (отрицательные -> 0)
    uint32_t errorCount();                          // ошибок обмена (после всех повторов)
    uint32_t retryCount();                          // повторов всего (неудачи, исправленные повтором)
    void     statusText(char* buf, size_t len);     // состояние для веба: «OK, 812 ppm, ...»
    // Ошибки (экран, веб; каждая пишется и в журнал ошибок)
    Co2Err   lastError();                           // последний опрос: None - ответ верный и датчик без ошибок
    uint16_t sensorStatus();                        // статус от датчика: S8 - MeterStatus (IR1), K22 - Error Status
                                                    // (RAM 0x1E); 0 - без ошибок
    void     statusBitsText(uint16_t status, char* buf, size_t len);   // расшифровка статуса
    void     faultText(char* buf, size_t len);      // кратко для шапки экрана: «нет связи», «ERR x23», «ERR Mdb-3»
    uint8_t  failRun();                             // неудачных опросов подряд (любая ошибка, и статус датчика)

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

#if CO2_SENSOR == CO2_SENSOR_K22
    // Тест I2C (i2c_test.cpp, на время теста update() не вызывается): запрос CO2 и чтение ответа без автомата опроса.
    bool    testRequest();                          // запрос CO2 принят (ACK)
    int8_t  testRead(uint32_t now, Co2ReadFail& why);   // 1 - CO2 прочитан (значение - как от опроса), 0 - не готов (why), -1 - неверный
    void    testEnd();                              // после теста: прерванный сеанс опроса - заново
#endif
}
