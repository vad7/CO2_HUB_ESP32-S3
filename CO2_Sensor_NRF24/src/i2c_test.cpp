// i2c_test.cpp - тест шины I2C (i2c_test.h). Неблокирующий: каждый вызов loop() делает по одному шагу сеансов
// (запрос / чтение - одна транзакция I2C, ~0.3..1 мс), ожидания - по esp_timer. Время транзакций - esp_timer_get_time()
// до и после вызова драйвера lgfx::i2c (обмен в нём - опросом, вызов возвращается по концу транзакции).
#include "i2c_test.h"
#include "config.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lgfx_include.h"
#include "i2c_bus.h"
#include "hub_config.h"
#include "co2_sensor.h"
#include "temp_backend.h"
#include "net.h"
#include "ui.h"

namespace I2cTest {

constexpr uint32_t US_PER_MS    = 1000;
constexpr uint32_t MS_PER_S     = 1000;
constexpr uint32_t CYCLE_US     = I2C_TEST_CYCLE_MS * US_PER_MS;
constexpr uint8_t  DELAY_COUNT  = sizeof(I2C_TEST_K22_DELAYS_MS) / sizeof(I2C_TEST_K22_DELAYS_MS[0]);
constexpr uint8_t  PASS_SPREAD  = 0;                // проход 1: опрос размазан по циклу
constexpr uint8_t  PASS_NORMAL  = 1;                // проход 2: как в работе
constexpr uint8_t  PASS_DELAYS  = 2;                // проход 3: K22 с паузами I2C_TEST_K22_DELAYS_MS
constexpr uint8_t  PASS_TIMED   = 2;                // проходов с временем по фазам (1, 2)
constexpr uint8_t  PASS_COUNT   = 3;
constexpr bool     HAVE_K22     = CO2_SENSOR == CO2_SENSOR_K22;
constexpr uint8_t  NAME_W       = 18;               // отчёт: ширина колонки названия, символов
constexpr uint8_t  NUM_W        = 9;                // отчёт: ширина колонки числа, символов
constexpr uint8_t  CELL_W       = 7;                // отчёт: ширина колонки таблицы прохода 3, символов
constexpr uint8_t  UTF8_CONT_MASK = 0xC0;           // байт продолжения UTF-8: 10xxxxxx
constexpr uint8_t  UTF8_CONT      = 0x80;
constexpr uint32_t CENT_PER_MS  = 100;              // мс с двумя знаками после точки

// Накопитель: мин / макс / сумма (мкс или штуки)
struct Acc {
    uint32_t min;       // наименьшее
    uint32_t max;       // наибольшее
    uint32_t n;         // значений
    uint64_t sum;       // сумма
};

enum Phase : uint8_t { PH_REQ, PH_WAIT, PH_RESP, PH_TOTAL, PH_COUNT };   // запрос, ожидание, ответ, весь сеанс
enum Dev : uint8_t { DEV_K22, DEV_SHT, DEV_TOUCH, DEV_COUNT };

// Проходы 1-2: статистика датчика
struct DevStat {
    Acc      ph[PH_COUNT];   // время фаз удачных сеансов, мкс (тач - только PH_TOTAL)
    Acc      reads;          // K22: чтений ответа за удачный сеанс, штук
    uint32_t sessions;       // сеансов начато
    uint32_t errReq;         // запрос / команда не приняты (NACK)
    uint32_t errResp;        // нет ответа: K22 - за весь сеанс tS, SHT40 и тач - NACK на чтение
    uint32_t errBad;         // неверный ответ: K22 - команда / сумма, SHT40 - CRC
};

// Проход 3: статистика K22 для одной паузы
struct DelayStat {
    uint32_t okAt[I2C_TEST_K22_READS];   // прочитано на k-м чтении (k = индекс + 1)
    uint32_t noAnswer;       // все чтения без ответа
    uint32_t reqFail;        // запрос не принят
    uint32_t bad;            // неверный ответ
    Acc      total;          // сеанс удачных, мкс
};

// Идущий сеанс датчика: запрос -> ожидание -> чтения ответа
struct Session {
    bool     busy;           // сеанс идёт (ждёт чтения)
    int64_t  t0;             // начало запроса, мкс (esp_timer)
    int64_t  reqEnd;         // конец запроса, мкс
    int64_t  next;           // следующее чтение не раньше, мкс
    uint32_t reqUs;          // длительность запроса, мкс
    uint8_t  reads;          // чтений сделано
};

static bool      s_requested = false;               // тест запрошен из веба (начнётся в loop())
static bool      s_running   = false;               // тест идёт
static uint8_t   s_pass      = PASS_SPREAD;         // текущий проход (PASS_*)
static uint8_t   s_delay     = 0;                   // проход 3: индекс паузы в I2C_TEST_K22_DELAYS_MS
static int64_t   s_passT0    = 0;                   // начало первого цикла прохода, мкс
static int32_t   s_cycle     = -1;                  // текущий цикл прохода (-1 - ещё не начат)
static bool      s_started[DEV_COUNT];              // датчик в текущем цикле уже опрошен
static int64_t   s_touchNext = 0;                   // проход 2: следующий опрос тача, мкс
// Данные идущего теста - в куче только на время теста (выделяет request(), освобождает finish())
struct Work {
    Session   sess[DEV_COUNT];                      // идущие сеансы (тач - одна транзакция, сеанса нет)
    DevStat   stat[PASS_TIMED][DEV_COUNT];          // проходы 1-2
    DelayStat dstat[DELAY_COUNT];                   // проход 3
};
static Work*     s_w         = nullptr;             // данные теста (nullptr - тест не идёт)
static uint8_t   s_shtAddr   = 0;                   // адрес SHT40 (0 - не выбран / не найден драйвером)
static uint32_t  s_khz       = 0;                   // частота шины на время теста, кГц
static int32_t   s_leftShown = -1;                  // показанный остаток, с (строка хода - при смене)
static char*     s_report    = nullptr;             // отчёт / строка хода (I2C_TEST_REPORT_LEN, куча; выделяется при первом
                                                    // тесте и остаётся до перезагрузки; nullptr - теста не было)
static size_t    s_len       = 0;                   // длина отчёта при сборке

static int64_t us() { return esp_timer_get_time(); }

static void accAdd(Acc& a, uint32_t v)
{
    if (a.n == 0 || v < a.min) a.min = v;
    if (v > a.max) a.max = v;
    a.sum += v;
    a.n++;
}

static bool haveDev(Dev d)
{
    switch (d) {
    case DEV_K22:   return HAVE_K22;
    case DEV_SHT:   return s_shtAddr != 0;
    default:        return HAS_TOUCH;
    }
}

static bool timed() { return s_pass < PASS_TIMED; }

// ------------------------------------------------------------------ сеансы
#if CO2_SENSOR == CO2_SENSOR_K22
static void k22Start()
{
    Session& s = s_w->sess[DEV_K22];
    const int64_t t0 = us();
    const bool ok = Co2Sensor::testRequest();
    const int64_t t1 = us();
    if (timed()) s_w->stat[s_pass][DEV_K22].sessions++;
    if (!ok) {
        if (timed()) s_w->stat[s_pass][DEV_K22].errReq++;
        else         s_w->dstat[s_delay].reqFail++;
        return;
    }
    const uint32_t first = timed() ? K22_TWAIT_MIN_US : I2C_TEST_K22_DELAYS_MS[s_delay] * US_PER_MS;
    s = { true, t0, t1, t1 + first, (uint32_t)(t1 - t0), 0 };
}

static void k22Step(uint32_t nowMs)
{
    Session& s = s_w->sess[DEV_K22];
    if (!s.busy || us() < s.next) return;
    Co2ReadFail why;
    const int64_t ta = us();
    const int8_t r = Co2Sensor::testRead(nowMs, why);
    const int64_t tb = us();
    s.reads++;
    if (r == 0) {
        const bool more = timed() ? tb - s.t0 < (int64_t)K22_SESSION_MAX_MS * US_PER_MS : s.reads < I2C_TEST_K22_READS;
        if (more) { s.next = tb + (timed() ? K22_TWAIT_MIN_US : I2C_TEST_K22_REREAD_MS * US_PER_MS); return; }
    }
    s.busy = false;
    if (timed()) {
        DevStat& st = s_w->stat[s_pass][DEV_K22];
        if (r < 0)       { st.errBad++; return; }
        if (r == 0)      { st.errResp++; return; }
        accAdd(st.ph[PH_REQ], s.reqUs);
        accAdd(st.ph[PH_WAIT], (uint32_t)(ta - s.reqEnd));   // с неудачными чтениями
        accAdd(st.ph[PH_RESP], (uint32_t)(tb - ta));
        accAdd(st.ph[PH_TOTAL], (uint32_t)(tb - s.t0));
        accAdd(st.reads, s.reads);
        return;
    }
    DelayStat& d = s_w->dstat[s_delay];
    if (r < 0)       d.bad++;
    else if (r == 0) d.noAnswer++;
    else {
        d.okAt[s.reads - 1]++;
        accAdd(d.total, (uint32_t)(tb - s.t0));
    }
}
#else
static void k22Start() {}
static void k22Step(uint32_t) {}
#endif

static void shtStart()
{
    Session& s = s_w->sess[DEV_SHT];
    DevStat& st = s_w->stat[s_pass][DEV_SHT];
    const int64_t t0 = us();
    const bool ok = Sht40::testCommand(s_shtAddr);
    const int64_t t1 = us();
    st.sessions++;
    if (!ok) { st.errReq++; return; }
    s = { true, t0, t1, t1 + (int64_t)SHT40_MEASURE_MS * US_PER_MS, (uint32_t)(t1 - t0), 0 };
}

static void shtStep()
{
    Session& s = s_w->sess[DEV_SHT];
    if (!s.busy || us() < s.next) return;
    const int64_t ta = us();
    const int8_t r = Sht40::testRead(s_shtAddr);
    const int64_t tb = us();
    s.busy = false;
    DevStat& st = s_w->stat[s_pass][DEV_SHT];
    if (r == 0) { st.errResp++; return; }
    if (r < 0)  { st.errBad++; return; }
    accAdd(st.ph[PH_REQ], s.reqUs);
    accAdd(st.ph[PH_WAIT], (uint32_t)(ta - s.reqEnd));
    accAdd(st.ph[PH_RESP], (uint32_t)(tb - ta));
    accAdd(st.ph[PH_TOTAL], (uint32_t)(tb - s.t0));
}

// Тач: одна транзакция, как LovyanGFX при касании (запись номера регистра, повторный старт, чтение байта)
static void touchPoll()
{
#if HAS_TOUCH
    DevStat& st = s_w->stat[s_pass][DEV_TOUCH];
    uint8_t reg = TOUCH_REG_TD_STATUS, v = 0;
    const int64_t t0 = us();
    const bool ok = lgfx::i2c::transactionWriteRead(I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, &v, 1, I2cBus::hz()).has_value();
    const int64_t t1 = us();
    st.sessions++;
    if (ok) accAdd(st.ph[PH_TOTAL], (uint32_t)(t1 - t0));
    else    st.errResp++;
#endif
}

// ------------------------------------------------------------------ отчёт
static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char* fmt, ...)
{
    if (s_len >= I2C_TEST_REPORT_LEN - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(s_report + s_len, I2C_TEST_REPORT_LEN - s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += (size_t)n < I2C_TEST_REPORT_LEN - s_len ? (size_t)n : I2C_TEST_REPORT_LEN - 1 - s_len;
}

static size_t chars(const char* s)                  // символов UTF-8 (для выравнивания колонок)
{
    size_t n = 0;
    for (; *s; s++) if (((uint8_t)*s & UTF8_CONT_MASK) != UTF8_CONT) n++;
    return n;
}

static void cell(const char* s, uint8_t w, bool left)   // колонка шириной w символов
{
    const size_t n = chars(s);
    if (left) out("%s", s);
    for (size_t i = n; i < w; i++) out(" ");
    if (!left) out("%s", s);
}

static void msText(uint64_t v, char* b, size_t len)   // мкс -> «12.34» мс
{
    const uint64_t c = (v + US_PER_MS / CENT_PER_MS / 2) / (US_PER_MS / CENT_PER_MS);
    snprintf(b, len, "%lu.%02lu", (unsigned long)(c / CENT_PER_MS), (unsigned long)(c % CENT_PER_MS));
}

static void accLine(const char* name, const Acc& a)
{
    char b[16];
    out("    ");
    cell(name, NAME_W, true);
    if (a.n == 0) { cell("-", NUM_W, false); out("\n"); return; }
    msText(a.min, b, sizeof(b));         cell(b, NUM_W, false);
    msText(a.sum / a.n, b, sizeof(b));   cell(b, NUM_W, false);
    msText(a.max, b, sizeof(b));         cell(b, NUM_W, false);
    out("\n");
}

static void devReport(Dev d, const DevStat& st)
{
    static const char* const PH_K22[PH_COUNT] = { "запрос", "ожидание", "ответ", "итого" };   // фазы K22 в отчёте
    static const char* const PH_SHT[PH_COUNT] = { "команда", "измерение", "чтение", "итого" };   // фазы SHT40 в отчёте
    switch (d) {
    case DEV_K22:   out("  K22 (CO2):"); break;
    case DEV_SHT:   out("  SHT40 (0x%02X):", s_shtAddr); break;
    default:
#if HAS_TOUCH
        out("  Тач FT6336G (регистр 0x%02X, 1 байт):", TOUCH_REG_TD_STATUS);
#endif
        break;
    }
    out(" сеансов %lu, удачных %lu\n", (unsigned long)st.sessions, (unsigned long)st.ph[PH_TOTAL].n);
    out("    ");
    cell("мс", NAME_W, true);
    cell("мин", NUM_W, false); cell("ср.", NUM_W, false); cell("макс", NUM_W, false);
    out("\n");
    if (d == DEV_TOUCH) {
        accLine("чтение", st.ph[PH_TOTAL]);
        out("    ошибок (NACK): %lu\n", (unsigned long)st.errResp);
        return;
    }
    const char* const* names = d == DEV_K22 ? PH_K22 : PH_SHT;
    for (uint8_t p = 0; p < PH_COUNT; p++) accLine(names[p], st.ph[p]);
    if (d == DEV_K22) {
        const Acc& r = st.reads;
        if (r.n) out("    чтений ответа за сеанс: мин %lu, ср. %lu.%lu, макс %lu\n", (unsigned long)r.min,
                     (unsigned long)(r.sum / r.n), (unsigned long)(r.sum * 10 / r.n % 10), (unsigned long)r.max);
        out("    ошибок: запрос %lu, нет ответа за %lu мс %lu, неверный ответ %lu\n", (unsigned long)st.errReq,
            (unsigned long)K22_SESSION_MAX_MS, (unsigned long)st.errResp, (unsigned long)st.errBad);
    } else {
        out("    ошибок: команда %lu, чтение (NACK) %lu, CRC %lu\n", (unsigned long)st.errReq,
            (unsigned long)st.errResp, (unsigned long)st.errBad);
    }
}

static void buildReport()
{
    s_len = 0;
    s_report[0] = '\0';
    char when[32];
    if (Net::timeValid()) {
        const time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(when, sizeof(when), "аптайм %lu с", (unsigned long)Net::uptimeS());
    }
    out("Тест I2C: %s, шина %lu кГц, плата %s, датчик CO2 %s\n", when, (unsigned long)s_khz, BOARD_NAME, CO2_SENSOR_NAME);
    out("Время - от начала до конца транзакции I2C (esp_timer); ожидания кратны проходу loop() (~1 мс).\n");
    if (HAVE_K22)
        out("K22: ожидание - от конца запроса до начала удачного чтения (вместе с неудачными чтениями).\n");
    if (!s_shtAddr) out("SHT40: не выбран или не найден драйвером - не проверялся.\n");
    static const char* const PASS_NAMES[PASS_TIMED] = {   // заголовки проходов 1-2 в отчёте
        "Проход 1: опрос размазан по циклу %lu мс (K22 - 0, SHT40 - 1/3, тач - 2/3 цикла)",
        "Проход 2: как в работе - K22 и SHT40 в начале цикла %lu мс, тач - каждые %lu мс",
    };
    for (uint8_t p = 0; p < PASS_TIMED; p++) {
        out("\n");
        out(PASS_NAMES[p], (unsigned long)I2C_TEST_CYCLE_MS, (unsigned long)TOUCH_POLL_MS);
        out(", датчики - по %u сеансов\n", I2C_TEST_SESSIONS);
        if (HAVE_K22) out("  K22: первое чтение и повторы через %lu мс (tWAIT мин.), до конца сеанса %lu мс (tS макс.)\n",
                          (unsigned long)(K22_TWAIT_MIN_US / US_PER_MS), (unsigned long)K22_SESSION_MAX_MS);
        for (uint8_t d = 0; d < DEV_COUNT; d++)
            if (haveDev((Dev)d)) devReport((Dev)d, s_w->stat[p][d]);
    }
    if (!HAVE_K22) return;
    out("\nПроход 3: K22, первое чтение через паузу, до %u чтений через %lu мс, по %u сеансов\n", I2C_TEST_K22_READS,
        (unsigned long)I2C_TEST_K22_REREAD_MS, I2C_TEST_SESSIONS);
    out("    ");
    cell("пауза, мс", CELL_W + 3, true);
    char b[16];
    for (uint8_t k = 0; k < I2C_TEST_K22_READS; k++) { snprintf(b, sizeof(b), "%u-е", k + 1); cell(b, CELL_W, false); }
    cell("нет", CELL_W, false); cell("запрос", CELL_W + 1, false); cell("неверн.", CELL_W + 1, false);
    cell("сеанс ср.", NUM_W + 2, false);
    out("\n");
    for (uint8_t i = 0; i < DELAY_COUNT; i++) {
        const DelayStat& d = s_w->dstat[i];
        out("    ");
        snprintf(b, sizeof(b), "%lu", (unsigned long)I2C_TEST_K22_DELAYS_MS[i]);
        cell(b, CELL_W + 3, true);
        for (uint8_t k = 0; k < I2C_TEST_K22_READS; k++) { snprintf(b, sizeof(b), "%lu", (unsigned long)d.okAt[k]); cell(b, CELL_W, false); }
        snprintf(b, sizeof(b), "%lu", (unsigned long)d.noAnswer); cell(b, CELL_W, false);
        snprintf(b, sizeof(b), "%lu", (unsigned long)d.reqFail);  cell(b, CELL_W + 1, false);
        snprintf(b, sizeof(b), "%lu", (unsigned long)d.bad);      cell(b, CELL_W + 1, false);
        if (d.total.n) msText(d.total.sum / d.total.n, b, sizeof(b));
        else           strlcpy(b, "-", sizeof(b));
        cell(b, NUM_W + 2, false);
        out("\n");
    }
    out("  1-е..%u-е - на каком чтении ответ получен; нет - ни одно чтение не дало ответа; запрос - не принят;\n"
        "  сеанс ср. - от начала запроса до конца удачного чтения.\n", I2C_TEST_K22_READS);
}

// ------------------------------------------------------------------ ход теста
static uint32_t cyclesTotal() { return (uint32_t)I2C_TEST_SESSIONS * (PASS_TIMED + (HAVE_K22 ? DELAY_COUNT : 0)); }

static uint32_t cyclesDone()
{
    const uint32_t before = s_pass < PASS_DELAYS ? s_pass : PASS_TIMED + s_delay;   // законченных проходов (пауз)
    return before * I2C_TEST_SESSIONS + (s_cycle > 0 ? (uint32_t)s_cycle : 0);
}

static void showProgress()
{
    const uint32_t done = cyclesDone(), total = cyclesTotal();
    const int32_t left = (int32_t)((total > done ? total - done : 0) * I2C_TEST_CYCLE_MS / MS_PER_S);
    if (left == s_leftShown) return;
    s_leftShown = left;
    char line[64];                                  // на экран; в отчёт - с началом I2C_TEST_RUNNING
    snprintf(line, sizeof(line), "Проход %u из %u, осталось ~%ld с",
             (unsigned)(s_pass + 1 + (s_pass == PASS_DELAYS ? s_delay : 0)), (unsigned)(HAVE_K22 ? PASS_TIMED + DELAY_COUNT : PASS_TIMED),
             (long)left);
    snprintf(s_report, I2C_TEST_REPORT_LEN, "%s%s", I2C_TEST_RUNNING, line);
    Ui::showTest(line);
}

static void begin()
{
    s_requested = false;
    s_running   = true;
    memset(s_w, 0, sizeof(*s_w));
    s_shtAddr = Cfg::co2.tempSensor == TEMP_SENSOR_SHT40 ? Sht40::address() : 0;
    s_khz     = I2cBus::hz() / HZ_PER_KHZ;
    s_pass    = PASS_SPREAD;
    s_delay   = 0;
    s_cycle   = -1;
    s_leftShown = -1;
    s_passT0  = us() + CYCLE_US;                    // первый цикл - через цикл: сеанс опроса датчиком закончится
    Serial.println("Тест I2C: начат");
}

static void finish()
{
    s_running = false;
    buildReport();
    if (HAVE_K22) {
#if CO2_SENSOR == CO2_SENSOR_K22
        Co2Sensor::testEnd();
#endif
    }
    Ui::endTest();
    Serial.print(s_report);
    heap_caps_free(s_w);                            // статистика уже в отчёте
    s_w = nullptr;
}

// Следующий проход (паузу прохода 3); false - тест окончен
static bool nextPass()
{
    if (s_pass == PASS_DELAYS) {
        if (++s_delay >= DELAY_COUNT) return false;
    } else if (++s_pass == PASS_DELAYS && !HAVE_K22) {
        return false;
    }
    s_cycle   = -1;
    s_passT0  = us();
    s_touchNext = s_passT0;
    return true;
}

// Слот датчика в цикле, мкс (проход 1 - свой слот каждому, иначе - начало цикла)
static uint32_t slotUs(Dev d) { return s_pass == PASS_SPREAD ? (uint32_t)d * (CYCLE_US / I2C_TEST_SPREAD_SLOTS) : 0; }

void loop(uint32_t now)
{
    if (!s_running) begin();
    k22Step(now);
    shtStep();
    const int64_t t = us();
    if (t < s_passT0) return;
    const int32_t c = (int32_t)((t - s_passT0) / CYCLE_US);
    if (c >= I2C_TEST_SESSIONS) {                   // проход окончен - когда закончатся сеансы
        if (s_w->sess[DEV_K22].busy || s_w->sess[DEV_SHT].busy) return;
        if (!nextPass()) finish();
        return;
    }
    if (c != s_cycle) {
        s_cycle = c;
        memset(s_started, 0, sizeof(s_started));
        showProgress();
    }
    const int64_t inCycle = t - s_passT0 - (int64_t)c * CYCLE_US;
    for (uint8_t i = 0; i < DEV_COUNT; i++) {
        const Dev d = (Dev)i;
        if (s_started[d] || !haveDev(d) || s_w->sess[d].busy || inCycle < slotUs(d)) continue;
        if (d == DEV_TOUCH && s_pass != PASS_SPREAD) continue;   // проход 2 - по своему периоду, проход 3 - без тача
        if (d != DEV_K22 && s_pass == PASS_DELAYS) continue;     // проход 3 - только K22
        s_started[d] = true;
        if (d == DEV_K22)      k22Start();
        else if (d == DEV_SHT) shtStart();
        else                   touchPoll();
    }
    if (s_pass == PASS_NORMAL && haveDev(DEV_TOUCH) && t >= s_touchNext) {
        s_touchNext += (int64_t)TOUCH_POLL_MS * US_PER_MS;
        touchPoll();
    }
}

// Память - только под тест, во внутренней RAM: в PSRAM мелкий блок дробил бы место под буфер истории
static void* alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void request()
{
    if (s_running || s_requested) return;
    if (!s_report) s_report = (char*)alloc(I2C_TEST_REPORT_LEN);
    if (!s_w) s_w = (Work*)alloc(sizeof(Work));
    if (!s_report || !s_w) {                        // без памяти тест не запускается
        heap_caps_free(s_w);
        s_w = nullptr;
        return;
    }
    snprintf(s_report, I2C_TEST_REPORT_LEN, "%sподготовка", I2C_TEST_RUNNING);   // веб сразу видит, что тест идёт
    s_requested = true;
}


bool active()              { return s_requested || s_running; }
bool running()             { return s_running; }
const char* report()       { return s_report ? s_report : ""; }

} // namespace I2cTest
