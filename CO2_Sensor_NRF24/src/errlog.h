// errlog.h - журнал ошибок: кольцевой буфер в RAM (ERRLOG_BYTES), запись - время, устройство, код, аргумент.
// Та же ошибка (устройство, код, аргумент) среди последних ERRLOG_MERGE_DEPTH записей новой записи не создаёт:
// растут счётчик и время последнего раза - частые повторы (датчик молчит при каждом опросе) не вытесняют журнал.
// Расшифровка - при выводе (экран, веб). Вызовы - под Cfg::lock() (loop, веб); в setup() - до запуска веба.
// После перезапуска журнал пуст.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum class ErrDev : uint8_t { Co2, Temp, Radio, Fan, Touch, Count };

// Коды по устройствам; в скобках - аргумент записи
enum class Co2Err   : uint8_t { None, NoAnswer /* K22: Co2ReadFail - почему последнее чтение ответа неудачно */,
                                BadFrame, Exception /* код Modbus */, Sensor /* статус датчика */, Stale /* - */,
                                ReqFail /* K22: запрос не принят (запись по I2C) */ };
// K22, «нет ответа»: что дало последнее из K22_READ_RETRIES чтений ответа
enum class Co2ReadFail : uint8_t { Unknown, Nack /* NACK или обрыв обмена (датчик занят) */,
                                   NotReady /* ответ без бита complete */ };
enum class RadioErr : uint8_t { NotFound /* - */, Reg /* регистр << 8 | значение */, RegMismatch /* то же */ };
enum class FanErr   : uint8_t { NoAck, NoResponse, AddrFail, Silent, Remote /* байт состояния */, Eeprom };
enum class TouchErr : uint8_t { ReadFail /* - */ };   // тач (ES3C28P): касание есть (INT), а чтение по I2C не удалось
// Датчик температуры: код - TempErr (temp_backend.h), аргумент - тип датчика (TEMP_SENSOR_*). Вентилятор: аргумент -
// номер вентилятора (с 0) в старшем байте, у Remote младший байт - состояние от вентилятора.

// Запись журнала. Повторы той же ошибки складываются в одну запись: firstS - когда случилась впервые, lastS -
// когда повторилась последний раз, count - сколько всего раз (так журнал хранит и редкое, и частое, не вытесняя
// одно другим). 14 байт данных, компилятор выравнивает до 16 (по uint32_t).
struct ErrEntry {
    uint32_t firstS;    // аптайм первого раза, с (дата и время - Net::epochFromUptime)
    uint32_t lastS;     // аптайм последнего повтора, с
    uint16_t count;     // сколько раз (не больше UINT16_MAX)
    uint16_t arg;       // подробность ошибки, зависит от устройства и кода (см. перечисления выше)
    uint8_t  dev;       // устройство - ErrDev
    uint8_t  code;      // код ошибки - Co2Err / TempErr / RadioErr / FanErr по устройству
};
static_assert(sizeof(ErrEntry) == 16, "ErrEntry - 16 байт (ERRLOG_BYTES / 16 записей)");

namespace ErrLog {
    void     add(ErrDev dev, uint8_t code, uint16_t arg = 0);
    uint16_t count();                                   // записей в журнале
    bool     get(uint16_t i, ErrEntry& e);              // i = 0 - самая новая
    uint32_t changes();                                 // меняется при каждом добавлении и очистке (перерисовка экрана)
    void     clear();                                   // очистить журнал (веб «Инфо»)
    // «CO2 S8: нет ответа» - устройство и ошибка без времени
    void     describe(uint8_t dev, uint8_t code, uint16_t arg, char* buf, size_t len);
    // «20.10 09:15:02 - 10:23:31 x210  CO2 S8: нет ответа» - время (timeRange) и что случилось (веб)
    void     format(const ErrEntry& e, char* buf, size_t len);
    // «20.10 09:15:02» (один раз) или «20.10 09:15:02 - 10:23:31 x210» (повторы; последний раз в другой день - с датой)
    void     timeRange(const ErrEntry& e, char* buf, size_t len);
    // время события: «20.10 09:15:02» (часы установлены; withDate = false - без даты) или «+1:02:03» от включения
    void     timeText(uint32_t uptimeS, bool withDate, char* buf, size_t len);
}
