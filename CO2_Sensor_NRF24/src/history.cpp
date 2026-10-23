// history.cpp - кольцевой буфер истории CO2 и температуры, время - прибавками к периоду (history.h).
// Память выделяется при старте и при смене срока / периода (configure, история очищается).
// Размер ограничивается свободной памятью: самый большой свободный блок PSRAM минус HISTORY_PSRAM_RESERVE
// (без PSRAM - внутренней RAM минус HISTORY_RAM_RESERVE).
#include "history.h"
#include "config.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace History {

static_assert(sizeof(HistoryRecord) == HISTORY_REC_BYTES, "размер записи истории");

static HistoryRecord* s_buf     = nullptr;          // буфер (PSRAM / RAM), кольцевой
static uint32_t       s_cap     = 0;                // вмещает записей
static uint32_t       s_wanted  = 0;                // заказано записей (сутки x период)
static uint32_t       s_head    = 0;                // индекс следующей записи
static uint32_t       s_count   = 0;                // записей сейчас (не больше s_cap)
static uint32_t       s_periodS = 1;                // период рассылки, с (интервал записей без прибавки)
static uint32_t       s_lastUp  = 0;                // аптайм самой новой записи, с
static uint32_t       s_added   = 0;                // записей добавлено всего (сдвиг номеров для HistoryCursor)
static uint32_t       s_gen     = 0;                // поколение буфера: clear / configure - позиции читателей недействительны
static bool           s_psram   = false;            // буфер в PSRAM

static bool havePsram() { return ESP.getPsramSize() > 0; }

static uint32_t caps() { return havePsram() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT; }

static size_t freeForHistory()
{
    const size_t block = heap_caps_get_largest_free_block(caps());
    const size_t reserve = havePsram() ? HISTORY_PSRAM_RESERVE : HISTORY_RAM_RESERVE;
    return block > reserve ? block - reserve : 0;
}

void clear()
{
    s_head = s_count = 0;
    s_gen++;
}

bool configure(uint16_t days, uint16_t periodS)
{
    if (periodS == 0) periodS = 1;
    s_periodS = periodS;
    s_wanted = (uint32_t)((uint64_t)days * S_PER_DAY_CFG / periodS);
    if (s_wanted < HISTORY_RECORDS_MIN) s_wanted = HISTORY_RECORDS_MIN;
    if (s_buf) heap_caps_free(s_buf);           // сначала освободить - тогда весь объём доступен
    s_buf = nullptr;
    s_psram = havePsram();
    uint32_t n = s_wanted;
    const uint32_t fit = (uint32_t)(freeForHistory() / HISTORY_REC_BYTES);
    if (n > fit) n = fit;
    if (n >= HISTORY_RECORDS_MIN) s_buf = (HistoryRecord*)heap_caps_malloc((size_t)n * HISTORY_REC_BYTES, caps());
    s_cap = s_buf ? n : 0;
    clear();
    Serial.printf("История: %lu сут при %u с -> нужно %lu записей, выделено %lu (%s), %lu б\n",
                  (unsigned long)days, periodS, (unsigned long)s_wanted, (unsigned long)s_cap, s_psram ? "PSRAM" : "RAM",
                  (unsigned long)bytes());
    return s_buf != nullptr;
}

static void put(uint8_t extraS, uint16_t co2, int16_t temp)
{
    s_buf[s_head] = { extraS, co2, temp };
    s_head = (s_head + 1) % s_cap;
    if (s_count < s_cap) s_count++;
    s_added++;
}

void add(uint16_t co2, int16_t tempTenths, uint32_t uptimeS)
{
    if (s_buf == nullptr) return;
    uint32_t extra = 0;                         // интервал сверх периода, с
    if (s_count > 0 && uptimeS > s_lastUp + s_periodS) extra = uptimeS - s_lastUp - s_periodS;
    if (extra > HISTORY_EXTRA_MAX_S) {          // долгий пропуск - записи «нет данных» через период
        const uint32_t fills = (extra - HISTORY_EXTRA_MAX_S + s_periodS - 1) / s_periodS;
        if (fills >= s_cap) {
            clear();                            // пропуск длиннее всего буфера - старые записи не нужны
            extra = 0;
        } else {
            for (uint32_t k = 0; k < fills; k++) put(0, HISTORY_NO_CO2, HISTORY_NO_TEMP);
            extra -= fills * s_periodS;
        }
    }
    put((uint8_t)extra, co2, tempTenths);
    s_lastUp = uptimeS;
}

uint32_t count()     { return s_count; }
uint32_t capacity()  { return s_cap; }
uint32_t wanted()    { return s_wanted; }
size_t   bytes()     { return (size_t)s_cap * HISTORY_REC_BYTES; }
bool     inPsram()   { return s_psram; }

uint32_t maxRecords()
{
    return (uint32_t)((freeForHistory() + bytes()) / HISTORY_REC_BYTES);
}

static uint32_t slot(uint32_t i) { return (s_head + s_cap - 1 - i) % s_cap; }

bool get(uint32_t i, HistoryPoint& p, HistoryCursor& c)
{
    if (i >= s_count) return false;
    uint32_t idx = 0, up = s_lastUp;            // по умолчанию - проход от самой новой записи
    if (c.valid && c.gen == s_gen) {
        const uint32_t shifted = c.index + (s_added - c.added);   // та же запись после добавления новых
        if (shifted <= i && shifted < s_count) { idx = shifted; up = c.uptimeS; }
    }
    for (; idx < i; idx++) {
        const uint32_t step = s_periodS + s_buf[slot(idx)].extraS;
        up = up > step ? up - step : 0;
    }
    const HistoryRecord& r = s_buf[slot(i)];
    p = { up, r.co2, r.temp };
    c = { i, up, s_added, s_gen, true };
    return true;
}

} // namespace History
