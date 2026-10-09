// history.cpp - кольцевые буферы истории CO2 и температуры (параллельные: один индекс - одно время).
// Память выделяется при старте и при смене срока / периода (configure, история очищается); буфер
// температуры - при выборе датчика температуры (setTemp), для старых записей - «нет данных».
// Размер ограничивается свободной памятью: самый большой свободный блок PSRAM минус HISTORY_PSRAM_RESERVE
// (без PSRAM - внутренней RAM минус HISTORY_RAM_RESERVE), с расчётом на оба буфера (8 байт на запись),
// чтобы буфер температуры поместился и позже.
// Мин / макс температуры по всему буферу - с обновлением при записи; пересчёт всего буфера только когда
// затирается запись с текущим минимумом или максимумом.
#include "history.h"
#include "config.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace History {

constexpr uint32_t REC_BYTES_BOTH = HISTORY_CO2_REC_BYTES + HISTORY_TEMP_REC_BYTES;
static_assert(sizeof(HistoryRecord) == HISTORY_CO2_REC_BYTES, "размер записи CO2");

static HistoryRecord* s_buf     = nullptr;
static int16_t*       s_temp    = nullptr;
static uint32_t       s_cap     = 0;
static uint32_t       s_wanted  = 0;
static uint32_t       s_head    = 0;   // индекс следующей записи
static uint32_t       s_count   = 0;
static uint32_t       s_tempCnt = 0;   // записей с температурой
static bool           s_psram   = false;
static int16_t        s_tmin    = 0;
static int16_t        s_tmax    = 0;
static bool           s_rangeDirty = true;   // мин / макс нужно пересчитать по всему буферу

static bool havePsram() { return ESP.getPsramSize() > 0; }

static uint32_t caps() { return havePsram() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT; }

static size_t freeForHistory()
{
    const size_t block = heap_caps_get_largest_free_block(caps());
    const size_t reserve = havePsram() ? HISTORY_PSRAM_RESERVE : HISTORY_RAM_RESERVE;
    return block > reserve ? block - reserve : 0;
}

static void freeAll()
{
    if (s_buf)  heap_caps_free(s_buf);
    if (s_temp) heap_caps_free(s_temp);
    s_buf = nullptr;
    s_temp = nullptr;
    s_cap = 0;
}

static void fillNoTemp()
{
    for (uint32_t i = 0; i < s_cap; i++) s_temp[i] = HISTORY_NO_TEMP;
}

void clear()
{
    s_head = s_count = s_tempCnt = 0;
    s_rangeDirty = true;
    if (s_temp) fillNoTemp();
}

bool configure(uint16_t days, uint16_t periodS, bool withTemp)
{
    if (periodS == 0) periodS = 1;
    s_wanted = (uint32_t)((uint64_t)days * S_PER_DAY_CFG / periodS);
    if (s_wanted < HISTORY_RECORDS_MIN) s_wanted = HISTORY_RECORDS_MIN;
    freeAll();                                  // сначала освободить - тогда весь объём доступен
    s_psram = havePsram();
    uint32_t n = s_wanted;
    const uint32_t fit = (uint32_t)(freeForHistory() / REC_BYTES_BOTH);
    if (n > fit) n = fit;
    if (n >= HISTORY_RECORDS_MIN) s_buf = (HistoryRecord*)heap_caps_malloc((size_t)n * HISTORY_CO2_REC_BYTES, caps());
    s_cap = s_buf ? n : 0;
    s_head = s_count = s_tempCnt = 0;
    s_rangeDirty = true;
    if (withTemp) setTemp(true);
    Serial.printf("История: %lu сут при %u с -> нужно %lu записей, выделено %lu (%s), CO2 %lu б, температура %lu б\n",
                  (unsigned long)days, periodS, (unsigned long)s_wanted, (unsigned long)s_cap, s_psram ? "PSRAM" : "RAM",
                  (unsigned long)co2Bytes(), (unsigned long)tempBytes());
    return s_buf != nullptr;
}

void setTemp(bool on)
{
    if (on == (s_temp != nullptr) || s_cap == 0) return;
    if (!on) {
        heap_caps_free(s_temp);
        s_temp = nullptr;
        s_tempCnt = 0;
        s_rangeDirty = true;
        return;
    }
    s_temp = (int16_t*)heap_caps_malloc((size_t)s_cap * HISTORY_TEMP_REC_BYTES, caps());
    if (!s_temp) { Serial.println("История: нет памяти под буфер температуры"); return; }
    fillNoTemp();                               // старые записи CO2 - без температуры
    s_tempCnt = 0;
    s_rangeDirty = true;
}

void add(uint16_t co2, int16_t tempTenths, uint32_t uptimeS)
{
    if (s_buf == nullptr) return;
    if (s_temp) {
        const int16_t old = s_temp[s_head];     // затираемая запись (при неполном буфере - «нет данных»)
        if (old != HISTORY_NO_TEMP) {
            s_tempCnt--;
            if (old == s_tmin || old == s_tmax) s_rangeDirty = true;
        }
        s_temp[s_head] = tempTenths;
        if (tempTenths != HISTORY_NO_TEMP) {
            if (s_tempCnt == 0) { s_tmin = s_tmax = tempTenths; s_rangeDirty = false; }
            else if (!s_rangeDirty) {
                if (tempTenths < s_tmin) s_tmin = tempTenths;
                if (tempTenths > s_tmax) s_tmax = tempTenths;
            }
            s_tempCnt++;
        }
    }
    s_buf[s_head].uptimeS = uptimeS;
    s_buf[s_head].co2     = co2;
    s_head = (s_head + 1) % s_cap;
    if (s_count < s_cap) s_count++;
}

uint32_t count()     { return s_count; }
uint32_t capacity()  { return s_cap; }
uint32_t wanted()    { return s_wanted; }
bool     hasTemp()   { return s_temp != nullptr; }
uint32_t tempCount() { return s_tempCnt; }
size_t   co2Bytes()  { return (size_t)s_cap * HISTORY_CO2_REC_BYTES; }
size_t   tempBytes() { return s_temp ? (size_t)s_cap * HISTORY_TEMP_REC_BYTES : 0; }
bool     inPsram()   { return s_psram; }

uint32_t maxRecords()
{
    return (uint32_t)((freeForHistory() + co2Bytes() + tempBytes()) / REC_BYTES_BOTH);
}

static uint32_t slot(uint32_t i) { return (s_head + s_cap - 1 - i) % s_cap; }

bool get(uint32_t i, HistoryRecord& rec)
{
    if (i >= s_count) return false;
    rec = s_buf[slot(i)];
    return true;
}

int16_t getTemp(uint32_t i)
{
    if (s_temp == nullptr || i >= s_count) return HISTORY_NO_TEMP;
    return s_temp[slot(i)];
}

bool tempRange(int16_t& lo, int16_t& hi)
{
    if (s_temp == nullptr || s_tempCnt == 0) return false;
    if (s_rangeDirty) {                         // пересчёт по всему буферу
        bool first = true;
        for (uint32_t i = 0; i < s_count; i++) {
            const int16_t t = s_temp[slot(i)];
            if (t == HISTORY_NO_TEMP) continue;
            if (first || t < s_tmin) s_tmin = t;
            if (first || t > s_tmax) s_tmax = t;
            first = false;
        }
        s_rangeDirty = false;
    }
    lo = s_tmin;
    hi = s_tmax;
    return true;
}

} // namespace History
