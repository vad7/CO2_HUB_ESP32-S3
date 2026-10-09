// history.h - история CO2 и температуры (график на экране, history.htm, history.csv).
//
// ДВА БУФЕРА - два отдельных массива одинаковой длины (capacity() записей), оба в PSRAM
// (если PSRAM нет - во внутренней RAM), выделяются heap_caps_malloc() в history.cpp:
//
//   буфер CO2          s_buf  : HistoryRecord[capacity]  - 6 байт на запись: аптайм (4) + CO2, ppm (2)
//   буфер температуры  s_temp : int16_t[capacity]        - 2 байта на запись: температура, 0.1 °C
//                                                          (HISTORY_NO_TEMP - в этот момент данных не было)
//
// Буферы ПАРАЛЛЕЛЬНЫЕ: общий указатель записи (head) и счётчик; ячейка k буфера температуры - это
// температура в момент записи ячейки k буфера CO2. Отдельного времени у температуры нет - оно берётся
// из записи CO2 с тем же номером. Кольцо: когда буфер полон, новая запись затирает самую старую.
//
// Когда пишется: раз в период рассылки (FanControl, Cfg::co2.transmitPeriodS) - CO2 и последнее
// достоверное значение датчика температуры (TempSensor) в один и тот же момент.
//
// Когда выделяется:
//   * буфер CO2 - при старте и при смене срока (Cfg::co2.historyDays) или периода рассылки:
//     configure() освобождает оба буфера, выделяет заново, история ОЧИЩАЕТСЯ;
//   * буфер температуры - пока в настройках выбран датчик температуры (setTemp(true) из loop());
//     ячейки за время до выбора - HISTORY_NO_TEMP; выбор снят - буфер освобождается (данные пропадают).
//
// Размер: записей = сутки * 86400 / период (не меньше HISTORY_RECORDS_MIN), но не больше, чем помещается
// в самый большой свободный блок памяти минус запас (HISTORY_PSRAM_RESERVE / HISTORY_RAM_RESERVE) из
// расчёта 8 байт на запись (6 + 2) - чтобы буфер температуры влез и позже. capacity() < wanted() -
// ограничено памятью.
//
// Чтение: get(i) / getTemp(i), i = 0 - самая новая запись. Реальное время записи - назад от текущих
// часов: Net::epochFromUptime(rec.uptimeS).
// Доступ - под Cfg::lock() (loop() и задача веб-сервера).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

struct HistoryRecord {
    uint32_t uptimeS;   // аптайм записи, с (Net::uptimeS); реальное время - назад от текущих часов (Net::epochFromUptime)
    uint16_t co2;       // ppm
} __attribute__((packed));

constexpr int16_t HISTORY_NO_TEMP = INT16_MIN;   // в записи нет температуры (датчик не выбран / нет данных)

namespace History {
    // (Пере)выделение по сроку и периоду; история очищается. withTemp - сразу и буфер температуры.
    // false - память не выделена совсем.
    bool     configure(uint16_t days, uint16_t periodS, bool withTemp);
    void     setTemp(bool on);                  // выделить / освободить буфер температуры (CO2 не трогается)
    void     clear();
    void     add(uint16_t co2, int16_t tempTenths, uint32_t uptimeS);   // tempTenths = HISTORY_NO_TEMP - нет
    uint32_t count();                           // записей CO2
    uint32_t capacity();                        // выделено записей
    uint32_t wanted();                          // нужно записей по сроку (capacity() < wanted() - ограничено памятью)
    uint32_t maxRecords();                      // сколько записей поместится в память сейчас (CO2 + температура)
    bool     hasTemp();                         // буфер температуры выделен
    uint32_t tempCount();                       // записей с температурой
    size_t   co2Bytes();                        // занято буфером CO2, байт
    size_t   tempBytes();                       // занято буфером температуры, байт
    bool     inPsram();                         // буферы в PSRAM
    // i = 0 - самая новая запись; false, если i >= count()
    bool     get(uint32_t i, HistoryRecord& rec);
    int16_t  getTemp(uint32_t i);               // HISTORY_NO_TEMP, если нет
    bool     tempRange(int16_t& lo, int16_t& hi);   // мин / макс температуры во всём буфере; false - данных нет
}
