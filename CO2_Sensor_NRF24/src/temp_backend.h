// temp_backend.h - общий интерфейс драйверов датчиков температуры (только для temp_sensor.cpp,
// ds18b20.cpp, sht40.cpp). Драйвер - автомат: start() начинает измерение, poll() вызывается из loop()
// и делает не больше одного короткого шага; Done - результат в err / tenths (0.1 °C) / rhTenths (0.1 %, -1 - датчик
// влажность не меряет).
#pragma once
#include <stdint.h>
#include <stddef.h>

enum class TempErr : uint8_t { None, NoData, LineLow, NoPresence, Crc, BadData, PowerOn, NoAck, Count };
enum class TempPoll : uint8_t { Busy, Done };

namespace Ds18b20 {
    void     attach();                                   // настроить вывод DQ
    void     detach();                                   // освободить вывод (смена типа датчика)
    void     start(uint32_t now);
    TempPoll poll(uint32_t now, TempErr& err, int16_t& tenths, int16_t& rhTenths);
    bool     serial(uint8_t* rom);                       // ROM (DS18B20_ROM_LEN байт, младший первым); false - не прочитан
    uint8_t  resolutionBits();                           // 9..12 (0 - ещё не читали)
    void     requestResolution(uint8_t bits);            // записать в датчик (EEPROM) перед следующим измерением
}

namespace Sht40 {
    void     attach();                                   // шина I2C (общая с K22 / тачем)
    void     detach();
    void     start(uint32_t now);
    TempPoll poll(uint32_t now, TempErr& err, int16_t& tenths, int16_t& rhTenths);
    uint8_t  address();                                  // найденный адрес I2C (0 - не найден)
    bool     serial(uint32_t& sn);                       // серийный номер; false - не прочитан
}
