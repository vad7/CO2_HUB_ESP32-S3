// sys_info.cpp — задачи FreeRTOS, загрузка ядер и температура кристалла. См. sys_info.h.
// Буферы — статические: снимок uxTaskGetSystemState (~1,3 КБ) и таблица строк; кучу не трогаем.
#include "sys_info.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if !configUSE_TRACE_FACILITY || !configGENERATE_RUN_TIME_STATS
#error "Нужна статистика задач FreeRTOS (Arduino-ESP32 3.x: CONFIG_FREERTOS_USE_TRACE_FACILITY, GENERATE_RUN_TIME_STATS)"
#endif
static_assert(SYSINFO_TASK_NAME_LEN == configMAX_TASK_NAME_LEN, "SYSINFO_TASK_NAME_LEN != configMAX_TASK_NAME_LEN");

namespace SysInfo {

constexpr uint8_t  CORES       = portNUM_PROCESSORS;
constexpr uint8_t  PCT         = 100;
constexpr uint16_t PERMILLE    = 1000;
constexpr float    TENTHS      = 10.0f;

// Строка таблицы + то, что нужно для накопления времени (номер задачи и прошлое значение счётчика)
struct Slot {
    TaskRow     row;
    UBaseType_t number;     // xTaskNumber — уникален для задачи
    uint32_t    prevCnt;    // ulRunTimeCounter в прошлом снимке
};

static TaskStatus_t s_status[SYSINFO_TASKS_MAX];   // снимок FreeRTOS
static Slot         s_slots[SYSINFO_TASKS_MAX];    // таблица (отсортирована)
static Slot         s_prev[SYSINFO_TASKS_MAX];     // прошлый снимок — для поиска задачи по номеру
static uint8_t      s_count     = 0;
static bool         s_overflow  = false;
static uint8_t      s_load[CORES];
static int16_t      s_chipT     = 0;
static bool         s_chipValid = false;
static uint32_t     s_last      = 0;
static int64_t      s_lastUs    = 0;
static uint32_t     s_snapId    = 0;

static const char* const STATE_NAMES[TASK_STATE_COUNT] = { "Выполняется", "Готова", "Ожидает", "Приостановлена", "Удалена" };
static const char* const STATE_SHORT[TASK_STATE_COUNT] = { "раб", "гот", "блк", "стп", "удл" };

static uint8_t mapState(eTaskState s)
{
    switch (s) {
    case eRunning:   return TASK_RUNNING;
    case eReady:     return TASK_READY;
    case eBlocked:   return TASK_BLOCKED;
    case eSuspended: return TASK_SUSPENDED;
    default:         return TASK_DELETED;
    }
}

// По убыванию приоритета, при равном — по имени
static bool before(const TaskRow& a, const TaskRow& b)
{
    if (a.prio != b.prio) return a.prio > b.prio;
    return strcmp(a.name, b.name) < 0;
}

static void snapshot()
{
    const int64_t nowUs = esp_timer_get_time();
    const uint64_t elapsedUs = (uint64_t)(nowUs - s_lastUs);
    const bool first = s_lastUs == 0;
    s_lastUs = nowUs;
    s_snapId++;

    uint32_t total = 0;   // в FreeRTOS — суммарное время; не используем: «Итого» считаем по накопленным суммам
    const UBaseType_t n = uxTaskGetSystemState(s_status, SYSINFO_TASKS_MAX, &total);
    s_overflow = n == 0 && uxTaskGetNumberOfTasks() > SYSINFO_TASKS_MAX;   // 0 — массив мал

    const uint8_t prevCount = s_count;
    memcpy(s_prev, s_slots, sizeof(Slot) * prevCount);

    uint64_t idleDeltaUs[CORES] = {};
    uint64_t sumUs = 0;
    s_count = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t& st = s_status[i];
        Slot& s = s_slots[s_count++];
        const Slot* old = nullptr;
        for (uint8_t k = 0; k < prevCount; k++)
            if (s_prev[k].number == st.xTaskNumber) { old = &s_prev[k]; break; }
        // приращение 32-битного счётчика без знака: верно и через переполнение (период << 71 мин);
        // новая задача — счётчик с её создания
        const uint32_t cnt = (uint32_t)st.ulRunTimeCounter;
        const uint32_t delta = old ? cnt - old->prevCnt : cnt;
        s.number  = st.xTaskNumber;
        s.prevCnt = cnt;
        strlcpy(s.row.name, st.pcTaskName, sizeof(s.row.name));
        s.row.state     = mapState(st.eCurrentState);
        s.row.prio      = (uint8_t)st.uxCurrentPriority;
        s.row.core      = st.xCoreID == tskNO_AFFINITY ? -1 : (int8_t)st.xCoreID;
        s.row.stackMinB = (uint32_t)st.usStackHighWaterMark;   // StackType_t = uint8_t -> байты
        s.row.timeUs    = (old ? old->row.timeUs : 0) + delta;
        sumUs += s.row.timeUs;
        for (uint8_t c = 0; c < CORES; c++)
            if (st.xHandle == xTaskGetIdleTaskHandleForCore(c)) idleDeltaUs[c] = delta;
    }
    for (uint8_t i = 0; i < s_count; i++)
        s_slots[i].row.shareTenths = sumUs ? (uint16_t)(s_slots[i].row.timeUs * PERMILLE / sumUs) : 0;

    // Сортировка вставками (~20 строк)
    for (uint8_t i = 1; i < s_count; i++) {
        const Slot t = s_slots[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && before(t.row, s_slots[j].row)) { s_slots[j + 1] = s_slots[j]; j--; }
        s_slots[j + 1] = t;
    }

    if (!first && elapsedUs)   // первый снимок — без загрузки (нет прошлого)
        for (uint8_t c = 0; c < CORES; c++) {
            const uint64_t idle = idleDeltaUs[c] > elapsedUs ? elapsedUs : idleDeltaUs[c];
            s_load[c] = (uint8_t)(PCT - idle * PCT / elapsedUs);
        }
}

void begin()
{
    s_last = millis() - SYSINFO_PERIOD_MS;   // первый снимок — сразу
}

void update(uint32_t now)
{
    if (now - s_last < SYSINFO_PERIOD_MS) return;
    s_last = now;
    snapshot();
    const float tc = temperatureRead();
    s_chipValid = !isnan(tc);
    if (s_chipValid) s_chipT = (int16_t)lroundf(tc * TENTHS);
}

uint8_t cpuLoad(uint8_t core)  { return core < CORES ? s_load[core] : 0; }
uint8_t cores()                { return CORES; }
bool    chipTempValid()        { return s_chipValid; }
int16_t chipTempTenths()       { return s_chipT; }
uint32_t snapshotId()          { return s_snapId; }
uint8_t taskCount()            { return s_count; }
bool    taskOverflow()         { return s_overflow; }
const TaskRow* task(uint8_t i) { return i < s_count ? &s_slots[i].row : nullptr; }
const char* stateName(uint8_t st)  { return st < TASK_STATE_COUNT ? STATE_NAMES[st] : "?"; }
const char* stateShort(uint8_t st) { return st < TASK_STATE_COUNT ? STATE_SHORT[st] : "?"; }

} // namespace SysInfo
