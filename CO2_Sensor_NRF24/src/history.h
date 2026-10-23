// history.h - история CO2 и температуры (график на экране, history.htm, history.csv).
//
// ОДИН БУФЕР - кольцо записей HistoryRecord (capacity() штук, HISTORY_REC_BYTES = 5 байт на запись) в PSRAM
// (если PSRAM нет - во внутренней RAM), выделяется heap_caps_malloc() в history.cpp. Когда буфер полон, новая запись
// затирает самую старую.
//
// Когда пишется: раз в период рассылки P (FanControl, Cfg::co2.transmitPeriodS) - CO2 и последнее достоверное
// значение датчика температуры (TempSensor) в один и тот же момент; датчика нет - HISTORY_NO_TEMP.
//
// Время записи не хранится целиком: известен аптайм самой новой записи, у каждой записи - на сколько секунд
// интервал от предыдущей длиннее P (extraS, 1 байт; обычно 0..1 с, при сбое датчика CO2 - пока не придёт верное
// значение, а опрос CO2 - не реже раза в CO2_POLL_MAX_S). Время более старых - назад от новой: t(i+1) = t(i) - P - extraS(i).
// Пропуск длиннее P + HISTORY_EXTRA_MAX_S (датчик долго молчал) заполняется записями «нет данных» (HISTORY_NO_CO2)
// через P - время не сбивается, на графике - разрыв. Смена P пересоздаёт буфер (configure), история очищается.
//
// Размер: записей = сутки * 86400 / P (не меньше HISTORY_RECORDS_MIN), но не больше, чем помещается в самый большой
// свободный блок памяти минус запас (HISTORY_PSRAM_RESERVE / HISTORY_RAM_RESERVE). capacity() < wanted() - ограничено
// памятью.
//
// Чтение: get(i, point, cursor), i = 0 - самая новая запись. Время восстанавливается проходом от новой записи;
// cursor (свой у каждого читателя) запоминает последнюю позицию - чтение подряд (i, i+1, ...) идёт без повторного
// прохода, даже если между вызовами добавились записи. Реальное время - Net::epochFromUptime(point.uptimeS).
// Доступ - под Cfg::lock() (loop() и задача веб-сервера).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Запись в буфере (HISTORY_REC_BYTES)
struct HistoryRecord {
    uint8_t  extraS;    // интервал от предыдущей записи сверх периода рассылки, с (0..HISTORY_EXTRA_MAX_S)
    uint16_t co2;       // ppm; HISTORY_NO_CO2 - нет данных (заполнение пропуска)
    int16_t  temp;      // температура, 0.1 °C; HISTORY_NO_TEMP - нет
} __attribute__((packed));

// Запись с восстановленным временем (результат get)
struct HistoryPoint {
    uint32_t uptimeS;   // аптайм записи, с (Net::uptimeS); реальное время - Net::epochFromUptime
    uint16_t co2;       // ppm; HISTORY_NO_CO2 - нет данных
    int16_t  temp;      // 0.1 °C; HISTORY_NO_TEMP - нет
};

// Позиция чтения (у каждого читателя своя; нулевая - чтение с самой новой записи)
struct HistoryCursor {
    uint32_t index;     // номер прочитанной записи (0 - самая новая) на момент чтения
    uint32_t uptimeS;   // её аптайм, с
    uint32_t added;     // записей добавлено всего на момент чтения (сдвиг номеров от новых записей)
    uint32_t gen;       // поколение буфера (clear / configure сбрасывают позицию)
    bool     valid;     // позиция задана
};

constexpr int16_t  HISTORY_NO_TEMP = INT16_MIN;    // в записи нет температуры (датчик не выбран / нет данных)
constexpr uint16_t HISTORY_NO_CO2  = UINT16_MAX;   // в записи нет CO2 (заполнение пропуска)

namespace History {
    // (Пере)выделение по сроку и периоду; история очищается. false - память не выделена совсем.
    bool     configure(uint16_t days, uint16_t periodS);
    void     clear();
    void     add(uint16_t co2, int16_t tempTenths, uint32_t uptimeS);   // tempTenths = HISTORY_NO_TEMP - нет
    uint32_t count();                           // записей
    uint32_t capacity();                        // выделено записей
    uint32_t wanted();                          // нужно записей по сроку (capacity() < wanted() - ограничено памятью)
    uint32_t maxRecords();                      // сколько записей поместится в память сейчас
    size_t   bytes();                           // выделено под буфер, байт
    bool     inPsram();                         // буфер в PSRAM
    // i = 0 - самая новая запись; false, если i >= count()
    bool     get(uint32_t i, HistoryPoint& p, HistoryCursor& c);
}
