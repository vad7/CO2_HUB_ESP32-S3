// ui.cpp — экран и настройка (параметры хаба, см. hub_config.h).
// Главный экран: CO2 крупными цифрами (цвета как на веб-странице хаба), время, ночь,
// скорость, поправка, статус вентиляторов.
// Экран горизонтальный (LCD_ROTATION = 1).
// Главный экран: шапка; цифры CO2; нижняя строка (иконки скорости, поправка «+1», справа температура и
//   влажность x2) — видна всегда. Без действий UI_MAIN_BUTTONS_MS активная панель скрывается.
//   ES (тач): касание — кнопки «+», «Меню», [график], «-», «Выход» над нижней строкой.
//   TD (КН — коротко, ДН — долго; Boot сверху, IO14 снизу): ДН Boot — меню, КН Boot — график,
//     КН IO14 — режим скорости (метки «+» у Boot и «-» у IO14): КН Boot «+», КН IO14 «-», ДН IO14 — выход.
// График истории CO2: ES — свайп, «<» «>», «-» «+» масштаб, «Выход»;
//   TD — КН IO14 к старым, КН Boot к новым, ДН Boot масштаб, ДН IO14 выход. UI_HISTORY_TIMEOUT_MS — выход.
// Настройки — по группам (MENU_GROUPS): карточка группы -> карточки «параметр — значение».
//   ES: < > — листание, «+» или касание карточки — открыть группу, в группе - + меняют значение сразу,
//     «Назад» / «Выход»; BOOT: КН — следующий, ДН — назад / выход.
//   TD: три уровня — группы, пункты, редактирование пункта. КН Boot — след. / «+», КН IO14 — пред. / «-»,
//     ДН Boot — войти (в группу / в редактирование), ДН IO14 — назад (из пункта / группы / меню).
// Выход из меню — также UI_SETUP_TIMEOUT_MS; изменения сохраняются в NVS при выходе.
// Ui::update() вызывается под Cfg::lock().
#include "ui.h"
#include <Arduino.h>
#include <stdio.h>
#include <time.h>
#include "lgfx_board.h"
#include "hub_config.h"
#include "co2_sensor.h"
#include "fan_control.h"
#include "net.h"
#include "history.h"
#include "build_info.h"
#include "temp_sensor.h"
#include "sys_info.h"
#include "font_digits.h"

static LGFX lcd;

namespace Ui {

// ---------------- Оформление ----------------
// Кириллица — шрифты U8g2 (в японских шрифтах IPA кириллица полноширинная: «В ы х о д»)
static const lgfx::U8g2font s_fontSmall(u8g2_font_9x15_t_cyrillic);    // 9x15
static const lgfx::U8g2font s_fontText (u8g2_font_10x20_t_cyrillic);   // 10x20
static const lgfx::U8g2font s_fontValue(u8g2_font_inr38_t_cyrillic);   // Inconsolata 38 px
static const lgfx::U8g2font s_fontValueMid(u8g2_font_inr24_t_cyrillic); // Inconsolata 24 px — если 38 не влезает
static const lgfx::IFont* const FONT_SMALL  = &s_fontSmall;
static const lgfx::IFont* const FONT_TEXT   = &s_fontText;
// Шрифты по убыванию — для подбора, чтобы значение меню влезло по ширине
static const lgfx::IFont* const VALUE_FONTS[] = { &s_fontValue, &s_fontValueMid, &s_fontText, &s_fontSmall };
static const lgfx::IFont* const LABEL_FONTS[] = { &s_fontText, &s_fontSmall };
static const lgfx::IFont* const FONT_DIGITS = &lgfx::fonts::Font7;   // 7-сегментные цифры 48 px

constexpr uint8_t  DIGITS_SCALE   = 2;     // 48 px * 2 = 96 px
constexpr int16_t  DIGITS_BAND_H  = 96;    // полоса крупных цифр: Font7 x2; сглаженные (font_digits.h) — по её центру
constexpr int16_t  DIGITS_LINE_MAX = LCD_WIDTH > LCD_HEIGHT ? LCD_WIDTH : LCD_HEIGHT;   // буфер строки: ширина в альбомной ориентации
constexpr int16_t  HEADER_H       = 26;
constexpr int16_t  INFO_LINE_H    = 22;
constexpr int16_t  BOTTOM_LINE_H  = 44;    // нижняя строка главного экрана: вентиляторы, поправка, температура x2
constexpr uint8_t  TEMP_TEXT_SCALE = 2;    // температура и влажность на главном экране: шрифт 10x20 x2
constexpr uint8_t  MAIN_BTN_COUNT = 5;     // ES: «+», «Меню», [график], «-», «Выход»
constexpr int16_t  ADJ_MARK_W     = 22;    // TD, режим скорости: метки «+» (у Boot) и «-» (у IO14) у левого края
constexpr int16_t  ADJ_MARK_H     = 28;
constexpr int16_t  BTN_ROW_H      = HAS_TOUCH ? 48 : 0;
constexpr int16_t  HINT_H         = HAS_TOUCH ? 0 : 20;
constexpr int16_t  EXIT_BTN_W     = 90;
constexpr int16_t  BTN_RADIUS     = 6;
constexpr int16_t  MARGIN         = 4;
constexpr int16_t  FAN_ICON_R     = 8;     // иконка вентилятора: радиус
constexpr int16_t  FAN_BLADE_DEG  = 50;    // ширина лопасти, градусы
constexpr int16_t  FAN_BLADES_DEG = 120;   // шаг лопастей (3 лопасти)
constexpr int16_t  CHART_ICON_W   = 28;    // иконка графика
constexpr int16_t  CHART_ICON_H   = 22;
constexpr int16_t  FAN_ICON_STEP  = 2 * FAN_ICON_R + 2;   // шаг иконок скорости в строке (промежуток 2 px: 6 иконок — 108 px)
constexpr int16_t  ARROW_W        = 22;    // стрелка «↔» в заголовке графика
constexpr int16_t  ARROW_HEAD     = 5;
constexpr int16_t  HDR_GAP        = 8;     // промежуток между частями заголовка графика
constexpr int16_t  STATUS_ICON_W  = 14;    // галочка / крестик статуса соединения
constexpr int16_t  STATUS_ICON_H  = 12;
constexpr int16_t  NET_LINE_H     = INFO_LINE_H;   // строка карточки «Сеть Wi-Fi»
constexpr uint8_t  SYS_LINES      = 6;     // карточка «Состояние системы»: строк (с названием)
constexpr int16_t  SYS_LINE_H     = 19;    //   высота строки (9x15)
constexpr size_t   SYS_LINE_BYTES = 64;    //   буфер строки: 34 символа на экране, кириллица — 2 байта
constexpr int16_t  TASK_LINE_H    = 15;    // карточка «Задачи FreeRTOS»: строка 9x15 (ES — заголовок + 9 задач, TD — + 6)
// Колонки таблицы задач, в знаках моноширинного 9x15 (34 знака на строку): имя 0..11, состояние с 13,
// правые края чисел — приоритет 19, стек 25, «итог» 30; страница «1/3» — справа в строке заголовка
constexpr uint8_t  TASK_NAME_COLS  = 12;
constexpr uint8_t  TASK_COL_STATE  = 13;
constexpr uint8_t  TASK_COL_PRIO_E = 19;
constexpr uint8_t  TASK_COL_STACK_E = 25;
constexpr uint8_t  TASK_COL_SHARE_E = 30;
constexpr uint8_t  TENTHS_PER_PCT  = 10;
constexpr size_t   INFO_LINE_BYTES = 96;   // строка карточек «Сеть Wi-Fi», «О программе» (пароль AP до 64 байт + подпись)
constexpr int16_t  DEG_R          = 2;     // значок градуса температуры: радиус
constexpr int16_t  DEG_DY         = 5;     //   выше середины строки
constexpr int16_t  DEG_GAP        = 2;     //   промежутки до цифр и «C»
constexpr int16_t  TENTHS_PER_DEG = 10;
constexpr int16_t  TEMP_RH_GAP    = 10;    // между температурой и влажностью
constexpr float    RH_TEXT_RATIO  = 0.8f;  // влажность — на 20 % мельче температуры (дробный масштаб шрифта LovyanGFX)
constexpr uint32_t TOUCH_REPEAT_DELAY_MS = 500;
constexpr uint32_t TOUCH_REPEAT_MS       = 120;
constexpr uint16_t PPM_STEP       = 10;    // шаг порогов и гистерезиса в меню
constexpr uint16_t HHMM_STEP_MIN  = 15;    // шаг времени ночи
constexpr uint16_t MIN_PER_DAY    = 24 * 60;
constexpr uint16_t MIN_PER_HOUR   = 60;
constexpr uint16_t HHMM_DIV       = 100;
constexpr uint8_t  FAN_MODE_COUNT = 3;     // 0 авто, 1 пропуск, 2 принудительно
constexpr uint32_t S_PER_MIN      = 60;
constexpr uint32_t S_PER_HOUR     = 3600;
constexpr uint32_t S_PER_DAY      = 86400;
// Размер истории в меню (записей)
// Размер истории в меню, сутки (в вебе — любое число HISTORY_DAYS_MIN..MAX)
static constexpr uint16_t HISTORY_DAYS_STEPS[] = { 1, 2, 3, 5, 7, 10, 14, 21, 30, 45, 60, 90, 120, 180, 270, 365 };
constexpr uint8_t HISTORY_DAYS_STEPS_COUNT = sizeof(HISTORY_DAYS_STEPS) / sizeof(HISTORY_DAYS_STEPS[0]);
static_assert(HISTORY_DAYS_STEPS[0] >= HISTORY_DAYS_MIN && HISTORY_DAYS_STEPS[HISTORY_DAYS_STEPS_COUNT - 1] <= HISTORY_DAYS_MAX, "Сроки истории вне диапазона");
constexpr uint32_t BYTES_PER_KB = 1024;
constexpr uint32_t BYTES_PER_MB = 1024 * 1024;
// График истории: записей на колонку (пиксель) — масштаб
static const uint16_t HIST_SCALES[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
constexpr uint8_t  HIST_SCALES_COUNT = sizeof(HIST_SCALES) / sizeof(HIST_SCALES[0]);
constexpr uint8_t  HIST_SCALE_DEF    = 2;     // 4 записи на пиксель: ~5 ч при 16 с
constexpr int16_t  HIST_Y_LABEL_W    = 44;    // поле подписей оси CO2 слева («2000» + засечка)
constexpr int16_t  HIST_X_LABEL_H    = 18;    // строка подписей времени снизу
constexpr int16_t  HIST_MAX_COLS     = 320;
constexpr int16_t  HIST_DRAG_MIN_PX  = 4;     // свайп: минимальный сдвиг для перерисовки
constexpr uint32_t HIST_DRAG_REDRAW_MS = 80;
constexpr uint16_t HIST_Y_MIN_SPAN   = 200;   // минимальный диапазон оси CO2, ppm
constexpr uint8_t  HIST_Y_MAX_LINES  = 5;     // линий сетки по CO2
static const uint16_t HIST_Y_STEPS[] = { 50, 100, 200, 250, 500, 1000, 2000 };
// Ось времени: шаг отметок, мин (круглые значения), не больше HIST_X_MAX_TICKS отметок на окно
static const uint16_t HIST_X_STEPS_MIN[] = { 1, 2, 5, 10, 15, 30, 60, 120, 180, 360, 720, 1440, 2880, 10080 };
constexpr uint8_t  HIST_X_MAX_TICKS  = 5;
constexpr int16_t  HIST_TICK_LEN     = 3;     // засечки осей, px
constexpr int16_t  HIST_LABEL_GAP    = 6;     // минимальный промежуток между подписями времени
constexpr int16_t  HIST_TAP_MAX_PX   = 8;     // касание без сдвига больше этого — выбор точки (курсор)
constexpr int16_t  HIST_CURSOR_R     = 3;     // точка курсора на кривой
constexpr int16_t  HIST_TIP_PAD      = 3;     // поля подсказки курсора
constexpr int16_t  HIST_T_MIN_SPAN   = 10;    // минимальный диапазон шкалы температуры, 0.1 °C

constexpr uint16_t COL_BG      = TFT_BLACK;
constexpr uint16_t COL_TEXT    = TFT_WHITE;
constexpr uint16_t COL_DIM     = TFT_DARKGREY;
constexpr uint16_t COL_OK      = TFT_GREEN;
constexpr uint16_t COL_WARN    = TFT_YELLOW;
constexpr uint16_t COL_ORANGE  = TFT_ORANGE;
constexpr uint16_t COL_ERR     = TFT_RED;
constexpr uint16_t COL_BTN     = TFT_NAVY;
constexpr uint16_t COL_HEADER  = TFT_CYAN;
constexpr uint16_t COL_AREA    = 0x4416;      // steelblue (70,130,180) — как график веб-страницы
constexpr uint16_t COL_AREA_TOP = TFT_SKYBLUE;
constexpr uint16_t COL_GRID    = 0x2104;      // тёмно-серый
constexpr uint16_t COL_AXIS    = TFT_LIGHTGREY;   // оси графика и подписи

// ---------------- Ввод ----------------
// Enter — TD: «войти глубже» (группа -> пункт -> редактирование); Hide — ES: «Выход» панели главного экрана;
// Adjust — TD: режим скорости на главном экране
enum class Key : uint8_t { None, Prev, Next, Dec, Inc, Menu, Exit, History, ShowButtons, Enter, Hide, Adjust };
enum class BtnEvent : uint8_t { None, Short, Long };
enum class Screen : uint8_t { Main, Menu, History };

struct Button {
    int8_t   pin;
    bool     raw;
    bool     pressed;
    bool     longFired;
    uint32_t changeTime;
    uint32_t pressTime;
};

static Button s_btnA = { BTN_PIN_A, false, false, false, 0, 0 };
static Button s_btnB = { BTN_PIN_B, false, false, false, 0, 0 };

struct TouchState {
    bool down; Key key; uint32_t downTime; uint32_t lastRepeat; uint32_t lastPoll;
    bool drag; int32_t lastX; uint32_t lastDragDraw;   // свайп по графику
    int32_t startX; bool moved;                         // касание графика без сдвига — курсор
    bool repeatOk;                                      // автоповтор удержанием (решается при нажатии)
};
static TouchState s_touch = { false, Key::None, 0, 0, 0, false, 0, 0, 0, false, false };

// ---------------- Меню ----------------
enum class Item : uint8_t {
    GlobalOverride, Night,
    FanSelect, FanSpeed, FanMode, FanMin, FanMax, FanDayMode, FanDayValue, FanNightMode, FanNightValue,
    FanChannel, FanAddress,
    NumberFans, Threshold1, Threshold2, Threshold3, Threshold4, Threshold5, Threshold6, SpeedDelta,
    NightStart, NightEnd, NightStartWd, NightEndWd, NightMax, BrightDay, BrightNight, TransmitPeriod, HistoryDays,
    RadioMode, PassiveChannel, NetInfo, RadioInfo, TouchCal, FactoryReset, About, TempPeriod, TempSensorType, SysState,
    DigitsFont, Tasks, WifiMode, WebPassReset,
    Count
};

// Группы меню (порядок пунктов внутри группы — порядок показа)
struct MenuGroup { const char* name; const Item* items; uint8_t count; };
static const Item G_CONTROL[] = { Item::GlobalOverride, Item::Night };
static const Item G_FANS[]    = { Item::NumberFans, Item::FanSelect, Item::FanSpeed, Item::FanMode, Item::FanMin, Item::FanMax,
                                  Item::FanDayMode, Item::FanDayValue, Item::FanNightMode, Item::FanNightValue,
                                  Item::FanChannel, Item::FanAddress };
static const Item G_CO2[]     = { Item::Threshold1, Item::Threshold2, Item::Threshold3, Item::Threshold4, Item::Threshold5,
                                  Item::Threshold6, Item::SpeedDelta };
static const Item G_NIGHT[]   = { Item::NightStart, Item::NightEnd, Item::NightStartWd, Item::NightEndWd, Item::NightMax };
static const Item G_RADIO[]   = { Item::RadioMode, Item::PassiveChannel, Item::TransmitPeriod, Item::RadioInfo };
static const Item G_SENSORS[] = { Item::HistoryDays, Item::TempSensorType, Item::TempPeriod };
static const Item G_SCREEN[]  = { Item::BrightDay, Item::BrightNight, Item::DigitsFont,
#if HAS_TOUCH
                                  Item::TouchCal,
#endif
                                };
static const Item G_SYSTEM[]  = { Item::SysState, Item::Tasks, Item::WifiMode, Item::NetInfo, Item::WebPassReset,
                                  Item::FactoryReset, Item::About };
#define MENU_GROUP(name, items) { name, items, (uint8_t)(sizeof(items) / sizeof(items[0])) }
static const MenuGroup MENU_GROUPS[] = {
    MENU_GROUP("Управление",        G_CONTROL),
    MENU_GROUP("Вентиляторы",       G_FANS),
    MENU_GROUP("Пороги CO2",        G_CO2),
    MENU_GROUP("Настройка ночи",    G_NIGHT),
    MENU_GROUP("Радио nRF24",       G_RADIO),
    MENU_GROUP("История и датчики", G_SENSORS),
    MENU_GROUP("Экран",             G_SCREEN),
    MENU_GROUP("Система",           G_SYSTEM),
};
#undef MENU_GROUP
constexpr uint8_t GROUP_COUNT = sizeof(MENU_GROUPS) / sizeof(MENU_GROUPS[0]);

static const char* const OVR_NAMES[OVR_COUNT]          = { "нет", "=", "+", "<=", ">=" };
static const char* const NIGHT_NAMES[NIGHT_OVR_COUNT]  = { "авто", "нет", "да" };
static const char* const FAN_MODE_NAMES[FAN_MODE_COUNT] = { "авто", "пропуск", "принуд." };
static const char* const RADIO_MODE_NAMES[RADIO_MODE_COUNT] = { "активный", "пассивный" };
static const char* const DIGITS_FONT_NAMES[DIGITS_FONT_COUNT] = { "7-сегментный", "сглаженный" };
static const char* const WIFI_MODE_NAMES[WIFI_MODE_COUNT] = { "выкл", "вкл", "часы" };

// ---------------- Состояние ----------------
static Screen   s_screen      = Screen::Main;
static uint8_t  s_group       = 0;      // группа меню (запоминается между входами)
static uint8_t  s_pos         = 0;      // пункт внутри группы
static bool     s_inGroup     = false;  // false — список групп, true — пункты группы
static bool     s_inItem      = false;  // TD: редактирование пункта (КН Boot / IO14 — «+» / «-»); у ES уровня нет
static uint8_t  s_fanSel      = 0;
static bool     s_resetArmed  = false;
static bool     s_resetDone   = false;
static bool     s_webPassDone = false;  // «Сброс пароля веба» выполнен (до выхода из меню)
static bool     s_dirtyNet    = false;  // Wi-Fi вкл/выкл, пароль веба — сохранить при выходе
static bool     s_dirtyCo2    = false;
static bool     s_dirtyFans   = false;
static bool     s_dirtyVars   = false;
static uint8_t  s_brightShown = 0;      // применённая яркость (0..255), 0 — ещё не задана
static bool     s_recalc      = false;
static uint32_t s_lastInput   = 0;
static uint32_t s_lastDraw    = 0;
static bool     s_radioRegsOk = false;
static uint16_t s_histDays    = 0;     // размер истории в меню, сутки (применяется при выходе)
static uint16_t s_menuPeriodS = 0;     // период рассылки при входе в меню (сменился — буферы пересоздаются)

// График истории
static uint32_t s_histOffset  = 0;     // записей от самой новой до правого края
static uint8_t  s_histScale   = HIST_SCALE_DEF;
static uint32_t s_histCount   = 0;     // History::count() при последней отрисовке
static uint16_t s_histCols[HIST_MAX_COLS];   // CO2 колонки (среднее), колонка 0 — правый край
static uint32_t s_histUp[HIST_MAX_COLS];     // аптайм самой новой записи колонки
static int16_t  s_histTemp[HIST_MAX_COLS];   // температура колонки (среднее), 0.1 °C; HISTORY_NO_TEMP — нет
static int16_t  s_histValid   = 0;     // колонок с данными
static int16_t  s_histCursor  = -1;    // колонка под курсором (-1 — нет)

// Кэш главного экрана — перерисовка только изменившегося
static int32_t  s_shownCo2    = -2;
static uint16_t s_shownColor  = 0;
static uint8_t  s_shownFont   = DIGITS_FONT_COUNT;   // шрифт показанных цифр (DigitsFont)
static char     s_shownHeader[64];
static char     s_shownInfo1[64];
static char     s_shownInfo2[128];
static bool     s_btnShown    = false;   // кнопки главного экрана показаны
static char     s_netShown[4 * INFO_LINE_BYTES];         // показанное состояние карточки «Сеть Wi-Fi» (перерисовка при изменении)
static uint8_t  s_taskPage    = 0;      // карточка «Задачи FreeRTOS»: страница
static uint32_t s_taskShown   = 0;      //   показанный снимок (SysInfo::snapshotId)
static char     s_sysShown[SYS_LINES * SYS_LINE_BYTES + SYS_LINES];   // показанное состояние карточки «Состояние системы»

// =====================================================================================
static int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Текущий пункт меню; Item::Count — открыт список групп
static Item curItem() { return s_inGroup ? MENU_GROUPS[s_group].items[s_pos] : Item::Count; }

// История: записей на days суток при текущем периоде рассылки (как History::configure)
static uint32_t histRecordsFor(uint16_t days)
{
    const uint16_t p = Cfg::co2.transmitPeriodS ? Cfg::co2.transmitPeriodS : 1;
    const uint32_t r = (uint32_t)((uint64_t)days * S_PER_DAY_CFG / p);
    return r < HISTORY_RECORDS_MIN ? HISTORY_RECORDS_MIN : r;
}

// Байт на запись истории: CO2 + температура, если датчик выбран
static uint32_t histRecBytes()
{
    return HISTORY_CO2_REC_BYTES + (Cfg::co2.tempSensor != TEMP_SENSOR_NONE ? HISTORY_TEMP_REC_BYTES : 0);
}

// Температура в 0.1 °C -> «23.4», «-5.0»
static void tenthsText(int16_t t, char* buf, size_t len)
{
    const int16_t a = t < 0 ? -t : t;
    snprintf(buf, len, "%s%d.%d", t < 0 ? "-" : "", a / TENTHS_PER_DEG, a % TENTHS_PER_DEG);
}

// Значок градуса: в шрифтах U8g2 *_t_cyrillic символа ° нет (Scripts/u8g2_glyphs.py) — в строке экрана
// на его месте пишется DEG_MARK, а drawStrDeg() рисует кружок радиусом DEG_R.
constexpr char    DEG_MARK = '`';
constexpr int16_t DEG_W    = 2 * DEG_R + 2 * DEG_GAP;   // место значка в строке, px

// Ширина строки с DEG_MARK в текущем шрифте
static int16_t degTextWidth(const char* s)
{
    char part[SYS_LINE_BYTES];
    int16_t w = 0;
    while (*s) {
        const char* e = strchr(s, DEG_MARK);
        const size_t n = e ? (size_t)(e - s) : strlen(s);
        strlcpy(part, s, n + 1 < sizeof(part) ? n + 1 : sizeof(part));
        w += (int16_t)lcd.textWidth(part);
        if (!e) break;
        w += DEG_W;
        s = e + 1;
    }
    return w;
}

// Строка с DEG_MARK от x, по центру cy (текущие шрифт и цвет; datum ставится middle_left)
static void drawStrDeg(const char* s, int16_t x, int16_t cy, uint16_t color)
{
    char part[SYS_LINE_BYTES];
    lcd.setTextDatum(lgfx::middle_left);
    const int16_t dy = (int16_t)lcd.fontHeight() / 2 - DEG_R - 1;   // кружок — у верха строчных букв
    while (*s) {
        const char* e = strchr(s, DEG_MARK);
        const size_t n = e ? (size_t)(e - s) : strlen(s);
        strlcpy(part, s, n + 1 < sizeof(part) ? n + 1 : sizeof(part));
        lcd.drawString(part, x, cy);
        x += (int16_t)lcd.textWidth(part);
        if (!e) break;
        lcd.drawCircle(x + DEG_GAP + DEG_R, cy - dy, DEG_R, color);
        x += DEG_W;
        s = e + 1;
    }
}

// Обрезать строку с конца по символам UTF-8 (не посреди русской буквы), пока она шире maxW в текущем шрифте
static void trimToWidth(char* s, int32_t maxW)
{
    size_t n = strlen(s);
    while (n > 0 && lcd.textWidth(s) > maxW) {
        do n--; while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80);   // назад через байты-продолжения 10xxxxxx
        s[n] = '\0';
    }
}

// Записей истории -> сутки при текущем периоде рассылки: «4.6»
static void histDaysText(uint32_t records, char* buf, size_t len)
{
    const uint32_t d10 = (uint32_t)((uint64_t)records * Cfg::co2.transmitPeriodS * 10 / S_PER_DAY_CFG);
    snprintf(buf, len, "%lu.%lu", (unsigned long)(d10 / 10), (unsigned long)(d10 % 10));
}

// Объём: «512 Б», «181 КБ», «2.3 МБ»
static void bytesText(uint64_t b, char* buf, size_t len)
{
    if (b < BYTES_PER_KB)      snprintf(buf, len, "%lu Б", (unsigned long)b);
    else if (b < BYTES_PER_MB) snprintf(buf, len, "%lu КБ", (unsigned long)((b + BYTES_PER_KB / 2) / BYTES_PER_KB));
    else {
        const uint32_t mb10 = (uint32_t)((b * 10 + BYTES_PER_MB / 2) / BYTES_PER_MB);
        snprintf(buf, len, "%lu.%lu МБ", (unsigned long)(mb10 / 10), (unsigned long)(mb10 % 10));
    }
}

static void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* text)
{
    lcd.fillRoundRect(x + MARGIN / 2, y + MARGIN / 2, w - MARGIN, h - MARGIN, BTN_RADIUS, COL_BTN);
    lcd.setFont(FONT_TEXT);
    lcd.setTextDatum(lgfx::middle_center);
    lcd.setTextColor(COL_TEXT, COL_BTN);
    lcd.drawString(text, x + w / 2, y + h / 2);
}

// Строка текста с очисткой фона
static void drawLine(const char* text, int16_t y, int16_t h, const lgfx::IFont* font, uint16_t color,
                     lgfx::textdatum_t datum = lgfx::middle_left, int16_t x = MARGIN)
{
    lcd.fillRect(0, y, lcd.width(), h, COL_BG);
    lcd.setFont(font);
    lcd.setTextDatum(datum);
    lcd.setTextColor(color, COL_BG);
    lcd.drawString(text, x, y + h / 2);
}

// =====================================================================================
// Главный экран
// =====================================================================================
// Раскладка: шапка | цифры CO2 | [ES: ряд кнопок, при активной панели] | нижняя строка (всегда видна)
static int16_t infoY2()   { return lcd.height() - BOTTOM_LINE_H; }               // нижняя строка
static int16_t mainBtnY() { return infoY2() - BTN_ROW_H; }                       // ES: кнопки — над нижней строкой
// Цифры (96 px): ES — между строкой статусов вентиляторов (под шапкой) и рядом кнопок, без наложения;
// TD — между шапкой и нижней строкой (100 px): строка статусов на TD не показывается (наложилась бы на цифры)
static int16_t digitsCY() { return HAS_TOUCH ? (HEADER_H + INFO_LINE_H + mainBtnY()) / 2 : (HEADER_H + infoY2()) / 2; }
static int16_t fansY()    { return HEADER_H; }                                   // строка скоростей вентиляторов — под часами
static int16_t mainBtnW() { return lcd.width() / MAIN_BTN_COUNT; }
static int16_t adjPlusY() { return fansY() + INFO_LINE_H + MARGIN; }             // TD: метка «+» — у Boot (сверху)
static int16_t adjMinusY(){ return infoY2() - ADJ_MARK_H - MARGIN; }             // TD: метка «-» — у IO14, над нижней строкой

// Иконка вентилятора: 3 лопасти + ступица
static void drawFanIcon(int16_t cx, int16_t cy, uint16_t color)
{
    for (int16_t a = 0; a < 360; a += FAN_BLADES_DEG)
        lcd.fillArc(cx, cy, FAN_ICON_R / 4, FAN_ICON_R, a, a + FAN_BLADE_DEG, color);
    lcd.fillCircle(cx, cy, FAN_ICON_R / 4, color);
}

// Иконка графика: оси + ломаная (точки в долях 1/8 размера иконки)
static void drawChartIcon(int16_t cx, int16_t cy, uint16_t color)
{
    static const uint8_t PX[] = { 1, 3, 4, 6, 8 };
    static const uint8_t PY[] = { 6, 3, 4, 1, 2 };
    constexpr int16_t PARTS = 8;
    const int16_t x = cx - CHART_ICON_W / 2, y = cy - CHART_ICON_H / 2;
    lcd.drawFastVLine(x, y, CHART_ICON_H, color);
    lcd.drawFastHLine(x, y + CHART_ICON_H - 1, CHART_ICON_W, color);
    for (uint8_t i = 0; i + 1 < sizeof(PX); i++) {
        int16_t x0 = x + PX[i] * (CHART_ICON_W - 1) / PARTS,     y0 = y + PY[i] * (CHART_ICON_H - 1) / PARTS;
        int16_t x1 = x + PX[i + 1] * (CHART_ICON_W - 1) / PARTS, y1 = y + PY[i + 1] * (CHART_ICON_H - 1) / PARTS;
        lcd.drawLine(x0, y0, x1, y1, color);
        lcd.drawLine(x0, y0 + 1, x1, y1 + 1, color);
    }
}

// Стрелка «↔» шириной ARROW_W от x, по центру cy (толщина 2 px)
static void drawArrowLR(int16_t x, int16_t cy, uint16_t color)
{
    const int16_t x1 = x + ARROW_W - 1;
    lcd.drawFastHLine(x, cy, ARROW_W, color);
    lcd.drawFastHLine(x, cy + 1, ARROW_W, color);
    for (int16_t d = 0; d <= 1; d++) {
        lcd.drawLine(x + d, cy, x + ARROW_HEAD + d, cy - ARROW_HEAD, color);
        lcd.drawLine(x + d, cy + 1, x + ARROW_HEAD + d, cy + 1 + ARROW_HEAD, color);
        lcd.drawLine(x1 - d, cy, x1 - ARROW_HEAD - d, cy - ARROW_HEAD, color);
        lcd.drawLine(x1 - d, cy + 1, x1 - ARROW_HEAD - d, cy + 1 + ARROW_HEAD, color);
    }
}

// Строка по центру с подбором шрифта: первый из fonts, в котором текст влезает по ширине
static void drawFitted(const char* text, int16_t y, int16_t h, const lgfx::IFont* const* fonts, size_t nFonts, uint16_t color)
{
    const int32_t maxW = lcd.width() - 2 * MARGIN;
    const lgfx::IFont* font = fonts[nFonts - 1];
    for (size_t i = 0; i < nFonts; i++) {
        lcd.setFont(fonts[i]);
        if (lcd.textWidth(text) <= maxW) { font = fonts[i]; break; }
    }
    drawLine(text, y, h, font, color, lgfx::middle_center, lcd.width() / 2);
}

// Активная панель главного экрана (s_btnShown): ES — касание экрана, TD — короткое IO14 (режим скорости).
// Показывает строку скоростей вентиляторов под часами и
//   ES: кнопки «+», «Меню», [график], «-», «Выход» — над нижней строкой (она остаётся видна);
//   TD: метки «+» у Boot (сверху) и «-» у IO14 (снизу) у левого края — поправка внизу видна.
static void showMainButtons(bool show)
{
    s_btnShown = show;
    s_shownInfo1[0] = s_shownInfo2[0] = '\0';   // строки перерисуются
    if (!show && HAS_TOUCH) lcd.fillRect(0, fansY(), lcd.width(), INFO_LINE_H, COL_BG);   // строка статусов (только ES)
#if HAS_TOUCH
    const int16_t w = mainBtnW(), y = mainBtnY();
    if (show) {
        drawButton(0,     y, w, BTN_ROW_H, "+");
        drawButton(w,     y, w, BTN_ROW_H, "Меню");
        drawButton(2 * w, y, w, BTN_ROW_H, "");
        drawChartIcon(2 * w + w / 2, y + BTN_ROW_H / 2, COL_TEXT);
        drawButton(3 * w, y, w, BTN_ROW_H, "-");
        drawButton(4 * w, y, lcd.width() - 4 * w, BTN_ROW_H, "Выход");
    } else {
        lcd.fillRect(0, y, lcd.width(), BTN_ROW_H, COL_BG);
    }
#else
    if (show) {
        drawButton(0, adjPlusY(),  ADJ_MARK_W, ADJ_MARK_H, "+");
        drawButton(0, adjMinusY(), ADJ_MARK_W, ADJ_MARK_H, "-");
    } else {
        lcd.fillRect(0, adjPlusY(),  ADJ_MARK_W, ADJ_MARK_H, COL_BG);
        lcd.fillRect(0, adjMinusY(), ADJ_MARK_W, ADJ_MARK_H, COL_BG);
    }
#endif
}

static void drawMainStatic()
{
    lcd.fillScreen(COL_BG);
    s_shownCo2 = -2;
    s_shownHeader[0] = s_shownInfo1[0] = s_shownInfo2[0] = '\0';
    s_btnShown = false;
}

static const char* fanStatusText(uint8_t fan, char* buf, size_t len)
{
    const CfgFan&   f  = Cfg::fans[fan];
    const FanState& st = FanControl::state(fan);
    const char* forced = (f.flags & FAN_FLAG_FORCED) ? "*" : "";
    if (f.flags & FAN_FLAG_SKIP) { snprintf(buf, len, "x"); return buf; }
    switch (st.txStatus) {
    case TX_OK:      snprintf(buf, len, "%u%s", st.speedCurrent, forced); break;
    case TX_ERROR:   snprintf(buf, len, "нет"); break;
    case TX_TIMEOUT: snprintf(buf, len, "nRF?"); break;   // модуль не завершил передачу за RADIO_TX_TIMEOUT_MS
    default:         snprintf(buf, len, "-"); break;
    }
    return buf;
}

static const char* netTag()
{
    switch (Net::mode()) {
    case Net::Mode::Station:      return "Wi-Fi";
    case Net::Mode::AccessPoint:
    case Net::Mode::StationAndAp: return "AP";
    case Net::Mode::Connecting:   return "Wi-Fi?";
    default:                      return "";
    }
}

// Температура текстом без единиц: «23.4», «-5.0»; нет данных — «--.-». Влажность: «45%», нет — «--%»,
// датчик без влажности (DS18B20) — пусто
static void tempText(char* t, size_t tLen, char* h, size_t hLen)
{
    const bool ok = TempSensor::valid();
    const int16_t v = TempSensor::tenthsC(), a = v < 0 ? -v : v;
    if (ok) snprintf(t, tLen, "%s%d.%d", v < 0 ? "-" : "", a / TENTHS_PER_DEG, a % TENTHS_PER_DEG);
    else    snprintf(t, tLen, "--.-");
    const int8_t rh = TempSensor::humidity();
    if (Cfg::co2.tempSensor != TEMP_SENSOR_SHT40) h[0] = '\0';
    else if (ok && rh >= 0) snprintf(h, hLen, "%d%%", rh);
    else                    snprintf(h, hLen, "--%%");
}

// Ширина «23.4°  45%» шрифтом f: температура x k, влажность x k * RH_TEXT_RATIO
static int16_t tempWidth(const lgfx::IFont* f, uint8_t k, const char* t, const char* h)
{
    lcd.setFont(f);
    lcd.setTextSize(k);
    int16_t w = (int16_t)lcd.textWidth(t) + (DEG_GAP + 2 * DEG_R) * k;
    if (h[0]) {
        lcd.setTextSize(k * RH_TEXT_RATIO);
        w += (int16_t)lcd.textWidth(h) + TEMP_RH_GAP * k;
    }
    lcd.setTextSize(1);
    return w;
}

// «23.4°  45%» у правого края, по центру cy (значок градуса рисуется: в шрифте его нет; «C» не пишется).
// Температура — 10x20 x TEMP_TEXT_SCALE, влажность — на 20 % мельче (RH_TEXT_RATIO).
// Не помещается правее leftX (иконки скорости и поправка) — x1, затем 9x15.
static void drawTemp(const char* t, const char* h, int16_t cy, uint16_t color, int16_t leftX)
{
    const int16_t maxW = lcd.width() - MARGIN - leftX - TEMP_RH_GAP;
    const lgfx::IFont* f = FONT_TEXT;
    uint8_t k = TEMP_TEXT_SCALE;                 // множитель: размер шрифта, значка градуса и промежутков
    if (tempWidth(f, k, t, h) > maxW) {
        k = 1;
        if (tempWidth(f, k, t, h) > maxW) f = FONT_SMALL;
    }
    lcd.setFont(f);
    lcd.setTextDatum(lgfx::middle_right);
    lcd.setTextColor(color, COL_BG);
    int16_t x = lcd.width() - MARGIN;
    if (h[0]) {
        lcd.setTextSize(k * RH_TEXT_RATIO);
        lcd.drawString(h, x, cy);
        x -= (int16_t)lcd.textWidth(h) + TEMP_RH_GAP * k;
    }
    lcd.setTextSize(k);
    x -= DEG_R * k;                              // центр значка градуса
    for (int16_t r = DEG_R * k; r > DEG_R * k - (int16_t)k; r--) lcd.drawCircle(x, cy - DEG_DY * k, r, color);   // толщина k
    lcd.drawString(t, x - (DEG_R + DEG_GAP) * k, cy);
    lcd.setTextSize(1);
}

// ---------------- Крупные цифры сглаженным шрифтом (font_digits.h) ----------------
static const SmoothGlyph* smoothGlyph(const SmoothFont& f, char c)
{
    const char* p = c ? strchr(SMOOTH_DIGITS_CHARS, c) : nullptr;
    return p ? &f.glyphs[p - SMOOTH_DIGITS_CHARS] : nullptr;
}

// Ширина чернил строки (от левого края первого растра до правого края последнего), px
static int16_t smoothTextWidth(const SmoothFont& f, const char* s)
{
    int16_t pen = 0, left = 0, right = 0;
    bool first = true;
    for (; *s; s++) {
        const SmoothGlyph* g = smoothGlyph(f, *s);
        if (!g) continue;
        if (first) { left = pen + g->dx; first = false; }
        right = pen + g->dx + g->w;
        pen += g->adv;
    }
    return right - left;
}

// Цвет RGB565 между bg (a = 0) и fg (a = SMOOTH_DIGITS_ALPHA_MAX)
static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a)
{
    const uint8_t n = SMOOTH_DIGITS_ALPHA_MAX;
    const uint16_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (n - a) + n / 2) / n;
    const uint16_t g = (((fg >> 5)  & 0x3F) * a + ((bg >> 5)  & 0x3F) * (n - a) + n / 2) / n;
    const uint16_t b = (( fg        & 0x1F) * a + ( bg        & 0x1F) * (n - a) + n / 2) / n;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Строка s по центру (cx, cy) на полосе DIGITS_BAND_H x ширина экрана: фон полосы — bg (как setTextPadding у Font7).
// Строки полосы собираются в буфер и выводятся pushImage — без мерцания, без кучи. false — не влезает по ширине.
static bool drawSmoothDigits(const char* s, int16_t cx, int16_t cy, uint16_t fg, uint16_t bg)
{
    static lgfx::rgb565_t line[DIGITS_LINE_MAX];
    const int16_t w = (int16_t)min<int32_t>(lcd.width(), DIGITS_LINE_MAX);
    const SmoothFont* f = &SMOOTH_DIGITS_BIG;
    int16_t tw = smoothTextWidth(*f, s);
    if (tw > w - 2 * MARGIN) { f = &SMOOTH_DIGITS_SMALL; tw = smoothTextWidth(*f, s); }   // 4 широкие цифры
    if (tw > w - 2 * MARGIN) return false;

    // Перо каждого знака: чернила строки по центру cx
    constexpr uint8_t MAX_CHARS = 8;
    const SmoothGlyph* gl[MAX_CHARS];
    int16_t gx[MAX_CHARS];
    uint8_t n = 0;
    int16_t pen = 0;
    for (; *s && n < MAX_CHARS; s++) {
        const SmoothGlyph* g = smoothGlyph(*f, *s);
        if (!g) continue;
        if (n == 0) pen = cx - tw / 2 - g->dx;
        gl[n] = g;
        gx[n++] = pen + g->dx;
        pen += g->adv;
    }
    uint16_t pal[SMOOTH_DIGITS_ALPHA_MAX + 1];
    for (uint8_t a = 0; a <= SMOOTH_DIGITS_ALPHA_MAX; a++) pal[a] = blend565(fg, bg, a);

    const int16_t top = cy - DIGITS_BAND_H / 2;
    const int16_t fontTop = (DIGITS_BAND_H - f->h) / 2;   // строка шрифта по центру полосы
    lcd.startWrite();
    for (int16_t y = 0; y < DIGITS_BAND_H; y++) {
        for (int16_t x = 0; x < w; x++) line[x] = pal[0];
        for (uint8_t i = 0; i < n; i++) {
            const SmoothGlyph* g = gl[i];
            const int16_t ry = y - fontTop - g->dy;
            if (ry < 0 || ry >= g->h) continue;
            const uint8_t* row = f->bitmap + g->offset + (uint32_t)ry * ((g->w + 1) / 2);
            for (int16_t rx = 0; rx < g->w; rx++) {
                const uint8_t a = (rx & 1) ? (row[rx / 2] & 0x0F) : (row[rx / 2] >> 4);
                const int16_t px = gx[i] + rx;
                if (a && px >= 0 && px < w) line[px] = pal[a];
            }
        }
        lcd.pushImage(0, top + y, w, 1, line);
    }
    lcd.endWrite();
    return true;
}

static void drawMainDynamic(uint32_t now)
{
    const bool     valid = Co2Sensor::isValid(now);
    const uint16_t co2   = Co2Sensor::co2();
    const uint8_t  nFans = Cfg::co2.fans;
    char buf[128];

    // --- Заголовок: время, ночь, статус ---
    char tbuf[8] = "--:--";
    if (Net::timeValid()) {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    const char* night = FanControl::nightEffective() ? " ночь" : "";
    uint8_t okCount = 0;
    for (uint8_t f = 0; f < nFans; f++) okCount += FanControl::state(f).txStatus == TX_OK;
    uint16_t hdrColor = COL_OK;
    if (!FanControl::radioOk()) {
        snprintf(buf, sizeof(buf), "%s%s  нет nRF24  %s", tbuf, night, netTag());
        hdrColor = COL_ERR;
    } else if (!valid) {
        snprintf(buf, sizeof(buf), "%s%s  нет датчика  %s", tbuf, night, netTag());
        hdrColor = COL_ERR;
    } else {
        // связь с вентиляторами: «связь 2/3» (на связи / всего), один — «связь: да / нет», нет вентиляторов — пусто
        char link[24] = "";
        if (nFans == 1)    snprintf(link, sizeof(link), "  связь: %s", okCount ? "да" : "нет");
        else if (nFans > 1) snprintf(link, sizeof(link), "  связь %u/%u", okCount, nFans);
        snprintf(buf, sizeof(buf), "%s%s%s  %s", tbuf, night, link, netTag());
        if (okCount != nFans) hdrColor = COL_WARN;
    }
    if (strcmp(buf, s_shownHeader) != 0) {
        strlcpy(s_shownHeader, buf, sizeof(s_shownHeader));
        drawLine(buf, 0, HEADER_H, FONT_TEXT, hdrColor);
    }

    // --- Крупные цифры ---
    int32_t  shown = valid || co2 != 0 ? (int32_t)co2 : -1;
    uint16_t color = !valid ? COL_DIM
                   : co2 <= CO2_LEVEL_GREEN  ? COL_OK
                   : co2 <= CO2_LEVEL_NORMAL ? COL_TEXT
                   : co2 <= CO2_LEVEL_ORANGE ? COL_ORANGE : COL_ERR;
    if (shown != s_shownCo2 || color != s_shownColor || Cfg::co2.digitsFont != s_shownFont) {
        s_shownCo2 = shown;
        s_shownColor = color;
        s_shownFont = Cfg::co2.digitsFont;
        if (shown < 0) snprintf(buf, sizeof(buf), "----");
        else           snprintf(buf, sizeof(buf), "%ld", (long)shown);
        // Сглаженный не влез (5 знаков — показание вне диапазона) — 7-сегментным, он уже
        if (s_shownFont != DIGITS_FONT_SMOOTH || !drawSmoothDigits(buf, lcd.width() / 2, digitsCY(), color, COL_BG)) {
            lcd.setFont(FONT_DIGITS);
            lcd.setTextSize(DIGITS_SCALE);
            lcd.setTextDatum(lgfx::middle_center);
            lcd.setTextColor(color, COL_BG);
            lcd.setTextPadding(lcd.width());
            lcd.drawString(buf, lcd.width() / 2, digitsCY());
            lcd.setTextPadding(0);
            lcd.setTextSize(1);
        }
        s_shownInfo1[0] = '\0';   // цифры могли затереть строку вентиляторов — перерисовать
        if (!HAS_TOUCH && s_btnShown) {   // TD: фон цифр на всю ширину стёр метки «+» / «-»
            drawButton(0, adjPlusY(),  ADJ_MARK_W, ADJ_MARK_H, "+");
            drawButton(0, adjMinusY(), ADJ_MARK_W, ADJ_MARK_H, "-");
        }
    }

    // --- Под часами: скорости вентиляторов — при активной панели, только ES (на TD наложилась бы на цифры) ---
    if (HAS_TOUCH && s_btnShown) {
        size_t pos = 0;
        char st[8];
        buf[0] = '\0';
        for (uint8_t f = 0; f < nFans && pos < sizeof(buf); f++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%u:%s ", f + 1, fanStatusText(f, st, sizeof(st)));
        if (strcmp(buf, s_shownInfo1) != 0) {
            strlcpy(s_shownInfo1, buf, sizeof(s_shownInfo1));
            drawLine(buf, fansY(), INFO_LINE_H, FONT_SMALL, COL_TEXT);
        }
    }
    // --- Нижняя строка (видна всегда, кнопки ES — над ней): скорость = число иконок вентилятора (0 — пусто),
    //     поправка «+1» — если ≠ 0; справа — температура и влажность (если датчик выбран в настройках) ---
    const uint8_t speed = FanControl::baseSpeed();
    char ovr[8] = "", temp[12] = "", hum[8] = "";
    if (Cfg::vars.speedOverride != 0) snprintf(ovr, sizeof(ovr), "%+d", Cfg::vars.speedOverride);
    const bool tempOk = TempSensor::valid();
    if (Cfg::co2.tempSensor != TEMP_SENSOR_NONE) tempText(temp, sizeof(temp), hum, sizeof(hum));
    snprintf(buf, sizeof(buf), "%u|%s|%s|%s", speed, ovr, temp, hum);   // ключ кэша
    if (strcmp(buf, s_shownInfo2) != 0) {
        strlcpy(s_shownInfo2, buf, sizeof(s_shownInfo2));
        const int16_t cy = infoY2() + BOTTOM_LINE_H / 2;
        const int16_t textX = MARGIN + speed * FAN_ICON_STEP + (speed ? MARGIN : 0);
        drawLine(ovr, infoY2(), BOTTOM_LINE_H, FONT_TEXT, COL_ERR, lgfx::middle_left, textX);   // поправка — красным
        for (uint8_t i = 0; i < speed; i++)
            drawFanIcon(MARGIN + FAN_ICON_R + i * FAN_ICON_STEP, cy, COL_TEXT);
        if (temp[0]) {
            lcd.setFont(FONT_TEXT);
            drawTemp(temp, hum, cy, tempOk ? COL_TEXT : COL_DIM, (int16_t)(textX + (ovr[0] ? lcd.textWidth(ovr) : 0)));
        }
    }
}

// =====================================================================================
// График истории CO2 (как history.htm: область под кривой), прокрутка и масштаб
// =====================================================================================
static int16_t  histX0()      { return HIST_Y_LABEL_W; }
static int16_t  histY0()      { return HEADER_H + MARGIN; }
static int16_t  histY1()      { return lcd.height() - BTN_ROW_H - HINT_H - HIST_X_LABEL_H; }   // не включая
static int16_t  histCols()    { int16_t c = lcd.width() - MARGIN - histX0(); return c > HIST_MAX_COLS ? HIST_MAX_COLS : c; }
static uint32_t histWindow()  { return (uint32_t)histCols() * HIST_SCALES[s_histScale]; }
static int16_t  histSideBtnW(){ return (lcd.width() - EXIT_BTN_W) / 4; }

static uint32_t histMaxOffset()
{
    uint32_t n = History::count(), w = histWindow();
    return n > w ? n - w : 0;
}

// records > 0 — к более старым данным (влево), < 0 — к новым
static void histScroll(int32_t records)
{
    int64_t o = (int64_t)s_histOffset + records;
    if (o < 0) o = 0;
    if (o > (int64_t)histMaxOffset()) o = histMaxOffset();
    s_histOffset = (uint32_t)o;
}

// Время записи: дата/время после NTP, иначе «N мин назад»
static void histTimeText(uint32_t uptimeS, char* buf, size_t len)
{
    time_t t = Net::epochFromUptime(uptimeS);
    if (t) {
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(buf, len, "%02d.%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(buf, len, "-%lu мин", (unsigned long)((Net::uptimeS() - uptimeS) / S_PER_MIN));
    }
}

static void drawHistoryStatic()
{
    lcd.fillScreen(COL_BG);
#if HAS_TOUCH
    const int16_t s = histSideBtnW(), y = lcd.height() - BTN_ROW_H;
    drawButton(0,                  y, s,          BTN_ROW_H, "<");
    drawButton(s,                  y, s,          BTN_ROW_H, "-");
    drawButton(2 * s,              y, EXIT_BTN_W, BTN_ROW_H, "Выход");
    drawButton(2 * s + EXIT_BTN_W, y, s,          BTN_ROW_H, "+");
    drawButton(3 * s + EXIT_BTN_W, y, lcd.width() - 3 * s - EXIT_BTN_W, BTN_ROW_H, ">");
#else
    drawLine("Boot:нов IO14:стар ДН:масшт/выход", lcd.height() - HINT_H, HINT_H, FONT_SMALL, COL_DIM);
#endif
}

// Смещение местного времени от UTC, с (на момент now; переход на летнее время внутри окна не учитывается)
static int32_t localOffsetS(time_t now)
{
    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
    int32_t dd = lt.tm_yday - gt.tm_yday;
    if (dd > 1) dd = -1;            // переход года
    else if (dd < -1) dd = 1;
    return dd * (int32_t)S_PER_DAY + (lt.tm_hour - gt.tm_hour) * (int32_t)S_PER_HOUR + (lt.tm_min - gt.tm_min) * (int32_t)S_PER_MIN;
}

// Минута записи для оси времени: местное время (минут от 1970), до установки часов — минуты аптайма
static int64_t histMinute(uint32_t uptimeS, bool timeOk, int32_t offS)
{
    if (!timeOk) return uptimeS / S_PER_MIN;
    return ((int64_t)Net::epochFromUptime(uptimeS) + offS) / (int64_t)S_PER_MIN;
}

// Подпись отметки оси времени (minute — граница шага): «ЧЧ:ММ», для шага от суток — «ДД.ММ»;
// до установки часов — «-N мин» / «-N ч» от текущего момента
static void histTickText(int64_t minute, uint16_t stepMin, bool timeOk, int32_t offS, char* buf, size_t len)
{
    if (!timeOk) {
        int64_t ago = (int64_t)(Net::uptimeS() / S_PER_MIN) - minute;
        if (ago < 0) ago = 0;
        if (ago >= MIN_PER_HOUR && ago % MIN_PER_HOUR == 0) snprintf(buf, len, "-%ldч", (long)(ago / MIN_PER_HOUR));
        else                                                snprintf(buf, len, "-%ldм", (long)ago);
        return;
    }
    if (stepMin >= MIN_PER_DAY) {
        time_t t = (time_t)(minute * (int64_t)S_PER_MIN - offS);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(buf, len, "%02d.%02d", tm.tm_mday, tm.tm_mon + 1);
        return;
    }
    const int64_t m = minute % MIN_PER_DAY;
    snprintf(buf, len, "%02d:%02d", (int)(m / MIN_PER_HOUR), (int)(m % MIN_PER_HOUR));
}

static int16_t histColX(int16_t c) { return histX0() + histCols() - 1 - c; }   // колонка 0 — правый край

// Курсор: вертикальная линия, точка на кривой, подсказка «время  значение» вверху графика
static void drawHistCursor(int16_t y0, int16_t y1, int16_t yv)
{
    const int16_t c = s_histCursor, x = histColX(c);
    for (int16_t y = y0; y < y1; y += 2) lcd.drawPixel(x, y, COL_WARN);   // пунктир
    lcd.fillCircle(x, yv, HIST_CURSOR_R, COL_WARN);

    char t[24], tt[16] = "", buf[64];
    histTimeText(s_histUp[c], t, sizeof(t));
    if (s_histTemp[c] != HISTORY_NO_TEMP) {   // температура колонки: «  23.4C» (знака градуса в шрифте нет)
        char v[12];
        tenthsText(s_histTemp[c], v, sizeof(v));
        snprintf(tt, sizeof(tt), "  %s%cC", v, DEG_MARK);
    }
    if (HIST_SCALES[s_histScale] > 1) snprintf(buf, sizeof(buf), "%s  ср. %u ppm%s", t, s_histCols[c], tt);
    else                              snprintf(buf, sizeof(buf), "%s  %u ppm%s", t, s_histCols[c], tt);
    lcd.setFont(FONT_SMALL);
    const int16_t w = degTextWidth(buf) + 2 * HIST_TIP_PAD, h = (int16_t)lcd.fontHeight() + 2 * HIST_TIP_PAD;
    int16_t bx = clampI(x - w / 2, histX0(), histX0() + histCols() - w);
    const int16_t by = yv - HIST_CURSOR_R < y0 + h + 2 ? y1 - h - 1 : y0;   // вверху, если не закрывает точку
    lcd.fillRect(bx, by, w, h, COL_BG);
    lcd.drawRect(bx, by, w, h, COL_WARN);
    lcd.setTextColor(COL_WARN, COL_BG);
    drawStrDeg(buf, bx + HIST_TIP_PAD, by + h / 2, COL_WARN);
}

static void drawHistoryChart()
{
    const int16_t  x0 = histX0(), y0 = histY0(), y1 = histY1(), cols = histCols(), ph = y1 - y0;
    const uint16_t scale = HIST_SCALES[s_histScale];
    if (s_histOffset > histMaxOffset()) s_histOffset = histMaxOffset();
    s_histCount = History::count();

    // Усреднение по scale записей на колонку; колонка 0 — правый край (самые новые)
    int16_t  validCols = 0;
    uint16_t vmin = UINT16_MAX, vmax = 0;
    for (int16_t c = 0; c < cols; c++) {
        uint32_t first = s_histOffset + (uint32_t)c * scale, sum = 0, k = 0;
        int32_t  tsum = 0, tk = 0;
        HistoryRecord r;
        for (uint16_t j = 0; j < scale && History::get(first + j, r); j++) {
            if (j == 0) s_histUp[c] = r.uptimeS;
            sum += r.co2;
            k++;
            const int16_t t = History::getTemp(first + j);
            if (t != HISTORY_NO_TEMP) { tsum += t; tk++; }
        }
        if (k == 0) break;
        uint16_t v = (uint16_t)(sum / k);
        s_histCols[c] = v;
        s_histTemp[c] = tk ? (int16_t)(tsum / tk) : HISTORY_NO_TEMP;
        validCols = c + 1;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    s_histValid = validCols;
    if (s_histCursor >= validCols) s_histCursor = -1;

    // Заголовок: «История CO2  ↔ 5 ч  x4» — стрелка рисуется (в шрифте нет символа ↔)
    char buf[64];
    const uint32_t spanS = histWindow() * Cfg::co2.transmitPeriodS;
    if (spanS >= S_PER_DAY) snprintf(buf, sizeof(buf), "%lu сут  x%u", (unsigned long)(spanS / S_PER_DAY), scale);
    else                    snprintf(buf, sizeof(buf), "%lu ч  x%u", (unsigned long)((spanS + S_PER_HOUR / 2) / S_PER_HOUR), scale);
    static const char* const HIST_TITLE = "История CO2";
    drawLine(HIST_TITLE, 0, HEADER_H, FONT_TEXT, COL_HEADER);
    const int16_t ax = MARGIN + lcd.textWidth(HIST_TITLE) + HDR_GAP;
    drawArrowLR(ax, HEADER_H / 2, COL_HEADER);
    lcd.drawString(buf, ax + ARROW_W + HDR_GAP / 2, HEADER_H / 2);

    lcd.fillRect(0, HEADER_H, lcd.width(), y1 + HIST_X_LABEL_H - HEADER_H, COL_BG);
    if (validCols == 0) {
        lcd.setFont(FONT_TEXT);
        lcd.setTextDatum(lgfx::middle_center);
        lcd.setTextColor(COL_DIM, COL_BG);
        lcd.drawString("История пуста", lcd.width() / 2, (y0 + y1) / 2);
        return;
    }

    // Ось CO2: шаг сетки из HIST_Y_STEPS, не больше HIST_Y_MAX_LINES линий
    uint16_t lo = vmin, hi = vmax;
    if (hi - lo < HIST_Y_MIN_SPAN) { lo = lo > HIST_Y_MIN_SPAN / 2 ? lo - HIST_Y_MIN_SPAN / 2 : 0; hi = lo + HIST_Y_MIN_SPAN; }
    uint16_t step = HIST_Y_STEPS[0];
    for (uint16_t s : HIST_Y_STEPS) { step = s; if ((hi - lo) / s < HIST_Y_MAX_LINES) break; }
    lo = (uint16_t)(lo / step * step);
    hi = (uint16_t)((hi + step - 1) / step * step);
    if (hi == lo) hi = lo + step;
    auto yOf = [&](uint16_t v) -> int16_t {
        return (int16_t)(y1 - 1 - (int32_t)(v - lo) * (ph - 1) / (hi - lo));
    };

    lcd.setFont(FONT_SMALL);
    lcd.setTextColor(COL_AXIS, COL_BG);
    lcd.setTextDatum(lgfx::middle_right);
    for (uint32_t v = lo; v <= hi; v += step) {
        const int16_t y = yOf((uint16_t)v);
        lcd.drawFastHLine(x0, y, cols, COL_GRID);
        lcd.drawFastHLine(x0 - HIST_TICK_LEN - 1, y, HIST_TICK_LEN, COL_AXIS);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
        lcd.drawString(buf, x0 - HIST_TICK_LEN - 2, y);
    }

    // Ось времени: отметки на круглых значениях (шаг из HIST_X_STEPS_MIN по ширине окна)
    const bool    timeOk = Net::timeValid();
    const int32_t offS   = timeOk ? localOffsetS(time(nullptr)) : 0;
    const uint32_t spanMin = spanS / S_PER_MIN;
    uint16_t stepMin = HIST_X_STEPS_MIN[0];
    for (uint16_t s : HIST_X_STEPS_MIN) { stepMin = s; if (spanMin / s <= HIST_X_MAX_TICKS) break; }
    struct Tick { int16_t x; int64_t minute; };
    Tick ticks[HIST_X_MAX_TICKS * 2 + 2];
    uint8_t nTicks = 0;
    int64_t prevKey = histMinute(s_histUp[0], timeOk, offS) / stepMin;
    for (int16_t c = 1; c < validCols && nTicks < sizeof(ticks) / sizeof(ticks[0]); c++) {
        const int64_t key = histMinute(s_histUp[c], timeOk, offS) / stepMin;
        if (key != prevKey) {   // между колонками c (старше) и c-1 прошла граница шага
            ticks[nTicks++] = { histColX(c - 1), prevKey * stepMin };
            prevKey = key;
        }
    }
    int16_t lastLabelL = lcd.width();   // подписи справа налево, без наложения
    lcd.setTextDatum(lgfx::top_center);
    for (uint8_t i = 0; i < nTicks; i++) {
        const int16_t x = ticks[i].x;
        lcd.drawFastVLine(x, y0, ph, COL_GRID);
        lcd.drawFastVLine(x, y1, HIST_TICK_LEN, COL_AXIS);
        histTickText(ticks[i].minute, stepMin, timeOk, offS, buf, sizeof(buf));
        const int16_t w = (int16_t)lcd.textWidth(buf);
        const int16_t cx = clampI(x, w / 2, lcd.width() - 1 - w / 2);
        if (cx + w / 2 + HIST_LABEL_GAP > lastLabelL) continue;
        lcd.drawString(buf, cx, y1 + HIST_TICK_LEN);
        lastLabelL = cx - w / 2;
    }

    // Область под кривой
    for (int16_t c = 0; c < validCols; c++) {
        const int16_t x = histColX(c), y = yOf(s_histCols[c]);
        lcd.drawFastVLine(x, y, y1 - y, COL_AREA);
        lcd.drawPixel(x, y, COL_AREA_TOP);
    }

    // Температура — красная линия, своя шкала: мин..макс по ВСЕМУ буферу (подписи справа вверху / внизу)
    int16_t tlo, thi;
    if (History::tempRange(tlo, thi)) {
        if (thi - tlo < HIST_T_MIN_SPAN) { tlo -= HIST_T_MIN_SPAN / 2; thi = tlo + HIST_T_MIN_SPAN; }
        auto tyOf = [&](int16_t t) -> int16_t {
            return (int16_t)(y1 - 1 - (int32_t)(t - tlo) * (ph - 1) / (thi - tlo));
        };
        for (int16_t c = 0; c < validCols; c++) {
            if (s_histTemp[c] == HISTORY_NO_TEMP) continue;
            const int16_t x = histColX(c), y = tyOf(s_histTemp[c]);
            if (c + 1 < validCols && s_histTemp[c + 1] != HISTORY_NO_TEMP) {   // к соседней (более старой) колонке
                const int16_t y2 = tyOf(s_histTemp[c + 1]);
                lcd.drawLine(x, y, x - 1, y2, COL_ERR);
                lcd.drawLine(x, y + 1, x - 1, y2 + 1, COL_ERR);   // толщина 2 px
            } else {
                lcd.fillRect(x, y, 1, 2, COL_ERR);
            }
        }
        char tb[12];
        lcd.setFont(FONT_SMALL);
        lcd.setTextColor(COL_ERR, COL_BG);
        tenthsText(thi, tb, sizeof(tb));
        lcd.setTextDatum(lgfx::top_right);
        lcd.drawString(tb, x0 + cols - 2, y0 + 1);
        tenthsText(tlo, tb, sizeof(tb));
        lcd.setTextDatum(lgfx::bottom_right);
        lcd.drawString(tb, x0 + cols - 2, y1 - 2);
    }

    // Оси поверх сетки и области
    lcd.drawFastVLine(x0 - 1, y0, ph + 1, COL_AXIS);
    lcd.drawFastHLine(x0 - 1, y1, cols + 1, COL_AXIS);

    if (s_histCursor >= 0) drawHistCursor(y0, y1, yOf(s_histCols[s_histCursor]));
}

#if HAS_TOUCH
// Касание графика без сдвига: курсор на колонку под пальцем, повторно в ту же точку или вне данных — убрать
static void histTap(int32_t x)
{
    const int16_t c = (int16_t)(histColX(0) - x);
    if (x < histX0() || c < 0 || c >= s_histValid || c == s_histCursor) s_histCursor = -1;
    else                                                                s_histCursor = c;
    drawHistoryChart();
}
#endif

static void enterHistory()
{
    s_screen = Screen::History;
    s_histOffset = 0;
    s_histCursor = -1;
    drawHistoryStatic();
    drawHistoryChart();
}

static void exitHistory()
{
    s_screen = Screen::Main;
    drawMainStatic();
}

static void handleHistoryKey(Key k)
{
    if (k != Key::None) s_histCursor = -1;   // прокрутка и масштаб — данные под курсором другие
    switch (k) {
    case Key::Prev: histScroll((int32_t)(histWindow() / 2)); break;    // к старым
    case Key::Next: histScroll(-(int32_t)(histWindow() / 2)); break;   // к новым
    case Key::Dec:  if (s_histScale + 1 < HIST_SCALES_COUNT) s_histScale++; break;   // мельче
    case Key::Inc:  if (s_histScale > 0) s_histScale--; break;                       // крупнее
    case Key::Menu: s_histScale = (uint8_t)((s_histScale + 1) % HIST_SCALES_COUNT); break;   // кнопкой: по кругу
    case Key::Exit: exitHistory(); return;
    default: return;
    }
    drawHistoryChart();
}

// =====================================================================================
// Меню настроек
// =====================================================================================
static void exitMenu();

static bool isFanItem(Item it) { return it >= Item::FanSpeed && it <= Item::FanAddress; }

// Карточки-сведения: значение не меняется — на TD в редактирование не входят
static bool isInfoItem(Item it)
{
    return it == Item::NetInfo || it == Item::RadioInfo || it == Item::About || it == Item::SysState;
}

static uint16_t* nightField(Item it)
{
    switch (it) {
    case Item::NightStart:   return &Cfg::co2.nightStart;
    case Item::NightEnd:     return &Cfg::co2.nightEnd;
    case Item::NightStartWd: return &Cfg::co2.nightStartWd;
    case Item::NightEndWd:   return &Cfg::co2.nightEndWd;
    default:                 return nullptr;
    }
}

static uint16_t hhmmAdd(uint16_t hhmm, int16_t dMin)
{
    int16_t m = (int16_t)((hhmm / HHMM_DIV) * MIN_PER_HOUR + hhmm % HHMM_DIV) + dMin;
    m = (int16_t)((m % MIN_PER_DAY + MIN_PER_DAY) % MIN_PER_DAY);
    return (uint16_t)((m / MIN_PER_HOUR) * HHMM_DIV + m % MIN_PER_HOUR);
}

static uint8_t fanMode(const CfgFan& f)
{
    return (f.flags & FAN_FLAG_SKIP) ? 1 : (f.flags & FAN_FLAG_FORCED) ? 2 : 0;
}

static void itemLabel(Item it, char* buf, size_t len)
{
    switch (it) {
    case Item::GlobalOverride: snprintf(buf, len, "Общая поправка скорости"); break;
    case Item::Night:          snprintf(buf, len, "Ночной режим"); break;
    case Item::FanSelect:      snprintf(buf, len, "Выбор вентилятора"); break;
    case Item::FanSpeed:       snprintf(buf, len, "Скорость (коррекция)"); break;
    case Item::FanMode:        snprintf(buf, len, "Режим"); break;
    case Item::FanMin:         snprintf(buf, len, "Скорость мин."); break;
    case Item::FanMax:         snprintf(buf, len, "Скорость макс."); break;
    case Item::FanDayMode:     snprintf(buf, len, "Коррекция днём"); break;
    case Item::FanDayValue:    snprintf(buf, len, "Значение днём"); break;
    case Item::FanNightMode:   snprintf(buf, len, "Коррекция ночью"); break;
    case Item::FanNightValue:  snprintf(buf, len, "Значение ночью"); break;
    case Item::FanChannel:     snprintf(buf, len, "Радиоканал"); break;
    case Item::FanAddress:     snprintf(buf, len, "Адрес"); break;
    case Item::NumberFans:     snprintf(buf, len, "Количество вентиляторов"); break;
    case Item::SpeedDelta:     snprintf(buf, len, "Гистерезис снижения"); break;
    case Item::NightStart:     snprintf(buf, len, "Начало ночи (будни)"); break;
    case Item::NightEnd:       snprintf(buf, len, "Конец ночи (будни)"); break;
    case Item::NightStartWd:   snprintf(buf, len, "Начало ночи (вых.)"); break;
    case Item::NightEndWd:     snprintf(buf, len, "Конец ночи (вых.)"); break;
    case Item::NightMax:       snprintf(buf, len, "Макс. скорость ночью"); break;
    case Item::BrightDay:      snprintf(buf, len, "Яркость экрана днём"); break;
    case Item::BrightNight:    snprintf(buf, len, "Яркость экрана ночью"); break;
    case Item::DigitsFont:     snprintf(buf, len, "Шрифт цифр CO2"); break;
    case Item::TransmitPeriod: snprintf(buf, len, "Период рассылки"); break;
    case Item::RadioMode:      snprintf(buf, len, "Режим связи"); break;
    case Item::PassiveChannel: snprintf(buf, len, "Канал пассивного режима"); break;
    case Item::HistoryDays:    snprintf(buf, len, "Размер истории"); break;
    case Item::NetInfo:        snprintf(buf, len, "Сеть Wi-Fi"); break;
    case Item::SysState:       snprintf(buf, len, "Состояние системы"); break;
    case Item::Tasks:          snprintf(buf, len, "Задачи FreeRTOS"); break;
    case Item::RadioInfo:      snprintf(buf, len, "Регистры nRF24"); break;
    case Item::TouchCal:       snprintf(buf, len, "Калибровка тача"); break;
    case Item::FactoryReset:   snprintf(buf, len, "Сброс настроек"); break;
    case Item::WifiMode:         snprintf(buf, len, "Wi-Fi"); break;
    case Item::WebPassReset:   snprintf(buf, len, "Сброс пароля веба"); break;
    case Item::About:          snprintf(buf, len, "О программе"); break;
    case Item::TempPeriod:     snprintf(buf, len, "Период чтения температуры"); break;
    case Item::TempSensorType: snprintf(buf, len, "Датчик температуры"); break;
    default:
        snprintf(buf, len, "Порог скорости %u", (unsigned)((uint8_t)it - (uint8_t)Item::Threshold1 + 1));
        break;
    }
}

static void itemValue(Item it, char* buf, size_t len)
{
    const CfgFan&   f  = Cfg::fans[s_fanSel];
    const FanState& st = FanControl::state(s_fanSel);
    if (uint16_t* nf = nightField(it)) {
        snprintf(buf, len, "%02u:%02u", *nf / HHMM_DIV, *nf % HHMM_DIV);
        return;
    }
    switch (it) {
    case Item::GlobalOverride: snprintf(buf, len, "%+d", Cfg::vars.speedOverride); break;
    case Item::Night:          snprintf(buf, len, "%s, %s", NIGHT_NAMES[Cfg::nightOverride], FanControl::isNight() ? "ночь" : "день"); break;
    case Item::FanSelect:      snprintf(buf, len, "%u из %u", s_fanSel + 1, Cfg::co2.fans); break;
    case Item::FanSpeed:
        if (f.flags & FAN_FLAG_FORCED) {
            if (st.forcedTimeoutS) snprintf(buf, len, "%u (%lu мин)", st.speedCurrent, (unsigned long)(st.forcedTimeoutS / S_PER_MIN));
            else                   snprintf(buf, len, "%u (всегда)", st.speedCurrent);
        } else {
            snprintf(buf, len, "%u (авто)", st.speedCurrent);
        }
        break;
    case Item::FanMode:        snprintf(buf, len, "%s", FAN_MODE_NAMES[fanMode(f)]); break;
    case Item::FanMin:         snprintf(buf, len, "%d", f.speedMin); break;
    case Item::FanMax:         snprintf(buf, len, "%d", f.speedMax); break;
    case Item::FanDayMode:     snprintf(buf, len, "%s", OVR_NAMES[f.overrideDay]); break;
    case Item::FanDayValue:    snprintf(buf, len, "%d", f.speedDay); break;
    case Item::FanNightMode:   snprintf(buf, len, "%s", OVR_NAMES[f.overrideNight]); break;
    case Item::FanNightValue:  snprintf(buf, len, "%d", f.speedNight); break;
    case Item::FanChannel:     snprintf(buf, len, "%u", f.rfChannel); break;
    case Item::FanAddress:     snprintf(buf, len, "0x%02X", f.addressLsb); break;
    case Item::NumberFans:     snprintf(buf, len, "%u", Cfg::co2.fans); break;
    case Item::SpeedDelta:     snprintf(buf, len, "%u ppm", Cfg::co2.speedDelta); break;
    case Item::NightMax:       snprintf(buf, len, "%u", Cfg::co2.nightMaxSpeed); break;
    case Item::BrightDay:      snprintf(buf, len, "%u %%", Cfg::co2.brightDayPct); break;
    case Item::BrightNight:    snprintf(buf, len, "%u %%", Cfg::co2.brightNightPct); break;
    case Item::DigitsFont:     snprintf(buf, len, "%s", DIGITS_FONT_NAMES[Cfg::co2.digitsFont]); break;
    case Item::TransmitPeriod: snprintf(buf, len, "%u с", Cfg::co2.transmitPeriodS); break;
    case Item::RadioMode:      snprintf(buf, len, "%s", RADIO_MODE_NAMES[Cfg::co2.radioMode]); break;
    case Item::PassiveChannel: snprintf(buf, len, "%u", Cfg::co2.passiveChannel); break;
    case Item::TempPeriod:     snprintf(buf, len, "%u с", Cfg::co2.tempPeriodS); break;
    case Item::TempSensorType: snprintf(buf, len, "%s", TempSensor::typeName(Cfg::co2.tempSensor)); break;
    case Item::HistoryDays: {   // сутки (объём буферов CO2 + температуры); не помещается в память — сколько влезет
        const uint32_t rec = histRecordsFor(s_histDays), maxRec = History::maxRecords();
        char sz[16];
        if (rec <= maxRec) {
            bytesText((uint64_t)rec * histRecBytes(), sz, sizeof(sz));
            snprintf(buf, len, "%u сут (%s)", s_histDays, sz);
        } else {
            const uint32_t d10 = (uint32_t)((uint64_t)maxRec * Cfg::co2.transmitPeriodS * 10 / S_PER_DAY);
            snprintf(buf, len, "%u сут: влезет %lu.%lu", s_histDays, (unsigned long)(d10 / 10), (unsigned long)(d10 % 10));
        }
        break;
    }
    case Item::NetInfo:        Net::ipString(buf, len); break;
    case Item::SysState:       buf[0] = 0; break;   // рисуется drawSysInfo()
    case Item::Tasks:          buf[0] = 0; break;   // рисуется drawTasks()
    case Item::About:          snprintf(buf, len, "%s", FW_VERSION); break;   // рисуется drawAbout()
    case Item::RadioInfo:
        snprintf(buf, len, "%s", !FanControl::radioOk() ? "нет модуля" : s_radioRegsOk ? "OK" : "ОТЛИЧАЮТСЯ");
        break;
    case Item::TouchCal:
        snprintf(buf, len, "%s", !HAS_TOUCH ? "нет тача" : s_resetArmed ? "Точно? +" : "+ перезапуск");
        break;
    case Item::FactoryReset:
        snprintf(buf, len, "%s", s_resetDone ? "Выполнено" : s_resetArmed ? "Точно? +" : "нажмите +");
        break;
    case Item::WifiMode:
        snprintf(buf, len, "%s", WIFI_MODE_NAMES[Cfg::net.wifiMode < WIFI_MODE_COUNT ? Cfg::net.wifiMode : WIFI_MODE_OFF]);
        break;
    case Item::WebPassReset:   // пароль по умолчанию показывается после сброса
        if (s_webPassDone) snprintf(buf, len, "%s", NET_WEB_PASS_DEF);
        else               snprintf(buf, len, "%s", s_resetArmed ? "Точно? +" : "нажмите +");
        break;
    default:
        snprintf(buf, len, "%u ppm", Cfg::co2.thresholds[(uint8_t)it - (uint8_t)Item::Threshold1]);
        break;
    }
}

static uint8_t taskPages();   // карточка «Задачи FreeRTOS» — ниже, у drawTasks()

static void changeItem(Item it, int8_t dir)
{
    CfgFan& f = Cfg::fans[s_fanSel];
    if (uint16_t* nf = nightField(it)) {
        *nf = hhmmAdd(*nf, (int16_t)(dir * HHMM_STEP_MIN));
        s_dirtyCo2 = s_recalc = true;
        return;
    }
    switch (it) {
    case Item::GlobalOverride:
        Cfg::vars.speedOverride = (int8_t)clampI(Cfg::vars.speedOverride + dir, -FAN_SPEED_MAX, FAN_SPEED_MAX);
        s_dirtyVars = s_recalc = true;
        break;
    case Item::Night:
        Cfg::nightOverride = (uint8_t)((Cfg::nightOverride + NIGHT_OVR_COUNT + dir) % NIGHT_OVR_COUNT);
        s_recalc = true;
        break;
    case Item::FanSelect:
        s_fanSel = (uint8_t)clampI(s_fanSel + dir, 0, Cfg::co2.fans > 0 ? Cfg::co2.fans - 1 : 0);
        break;
    case Item::FanSpeed:       // как кнопки «+»/«-» веб-страницы: принудительно на FORCE_MINUTES_DEF
        FanControl::fanOverride(s_fanSel, dir > 0 ? 'p' : 'm', FORCE_MINUTES_DEF);
        break;
    case Item::FanMode: {
        uint8_t m = (uint8_t)((fanMode(f) + FAN_MODE_COUNT + dir) % FAN_MODE_COUNT);
        f.flags = m == 1 ? FAN_FLAG_SKIP : m == 2 ? FAN_FLAG_FORCED : 0;
        s_dirtyFans = s_recalc = true;
        break;
    }
    case Item::FanMin:         f.speedMin = (int8_t)clampI(f.speedMin + dir, 0, FAN_SPEED_MAX); s_dirtyFans = s_recalc = true; break;
    case Item::FanMax:         f.speedMax = (int8_t)clampI(f.speedMax + dir, 0, FAN_SPEED_MAX); s_dirtyFans = s_recalc = true; break;
    case Item::FanDayMode:     f.overrideDay   = (uint8_t)((f.overrideDay   + OVR_COUNT + dir) % OVR_COUNT); s_dirtyFans = s_recalc = true; break;
    case Item::FanNightMode:   f.overrideNight = (uint8_t)((f.overrideNight + OVR_COUNT + dir) % OVR_COUNT); s_dirtyFans = s_recalc = true; break;
    case Item::FanDayValue:    f.speedDay   = (int8_t)clampI(f.speedDay   + dir, -FAN_SPEED_MAX, FAN_SPEED_MAX); s_dirtyFans = s_recalc = true; break;
    case Item::FanNightValue:  f.speedNight = (int8_t)clampI(f.speedNight + dir, -FAN_SPEED_MAX, FAN_SPEED_MAX); s_dirtyFans = s_recalc = true; break;
    case Item::FanChannel:     f.rfChannel  = (uint8_t)clampI(f.rfChannel + dir, 0, RF_CHANNEL_MAX); s_dirtyFans = true; break;
    case Item::FanAddress:     f.addressLsb = (uint8_t)(f.addressLsb + dir); s_dirtyFans = true; break;
    case Item::NumberFans:
        Cfg::co2.fans = (uint8_t)clampI(Cfg::co2.fans + dir, 0, FANS_MAX);
        s_fanSel = (uint8_t)clampI(s_fanSel, 0, Cfg::co2.fans > 0 ? Cfg::co2.fans - 1 : 0);
        s_dirtyCo2 = s_recalc = true;
        break;
    case Item::SpeedDelta:
        Cfg::co2.speedDelta = (uint16_t)clampI((int16_t)Cfg::co2.speedDelta + dir * PPM_STEP, 0, INT16_MAX);
        s_dirtyCo2 = true;
        break;
    case Item::NightMax:
        Cfg::co2.nightMaxSpeed = (uint8_t)clampI(Cfg::co2.nightMaxSpeed + dir, 0, FAN_SPEED_MAX);
        s_dirtyCo2 = s_recalc = true;
        break;
    case Item::BrightDay:
        Cfg::co2.brightDayPct = (uint8_t)clampI(Cfg::co2.brightDayPct + dir * BRIGHT_STEP_PCT, BRIGHT_MIN_PCT, BRIGHT_MAX_PCT);
        s_dirtyCo2 = true;
        break;
    case Item::BrightNight:
        Cfg::co2.brightNightPct = (uint8_t)clampI(Cfg::co2.brightNightPct + dir * BRIGHT_STEP_PCT, BRIGHT_MIN_PCT, BRIGHT_MAX_PCT);
        s_dirtyCo2 = true;
        break;
    case Item::DigitsFont:     // по кругу; главный экран перерисует цифры по смене шрифта
        Cfg::co2.digitsFont = (uint8_t)((Cfg::co2.digitsFont + DIGITS_FONT_COUNT + dir) % DIGITS_FONT_COUNT);
        s_dirtyCo2 = true;
        break;
    case Item::TransmitPeriod:
        Cfg::co2.transmitPeriodS = (uint16_t)clampI((int16_t)Cfg::co2.transmitPeriodS + dir, TRANSMIT_PERIOD_MIN_S, TRANSMIT_PERIOD_MAX_S);
        s_dirtyCo2 = true;
        break;
    case Item::RadioMode:      // применяется сразу (FanControl перенастраивает модуль), в NVS — при выходе
        Cfg::co2.radioMode = (uint8_t)((Cfg::co2.radioMode + RADIO_MODE_COUNT + dir) % RADIO_MODE_COUNT);
        s_dirtyCo2 = true;
        break;
    case Item::PassiveChannel:
        Cfg::co2.passiveChannel = (uint8_t)clampI(Cfg::co2.passiveChannel + dir, 0, RF_CHANNEL_MAX);
        s_dirtyCo2 = true;
        break;
    case Item::TempPeriod:
        Cfg::co2.tempPeriodS = (uint8_t)clampI(Cfg::co2.tempPeriodS + dir, TEMP_PERIOD_MIN_S, TEMP_PERIOD_MAX_S);
        s_dirtyCo2 = true;
        break;
    case Item::TempSensorType: {   // по кругу: нет -> DS18B20 (если есть вывод) -> SHT40; применяется сразу
        uint8_t t = Cfg::co2.tempSensor;
        do t = (uint8_t)((t + TEMP_SENSOR_COUNT + dir) % TEMP_SENSOR_COUNT);
        while (t == TEMP_SENSOR_DS18B20 && !TEMP_DS18B20_AVAILABLE);
        Cfg::co2.tempSensor = t;
        s_dirtyCo2 = true;
        break;
    }
    case Item::HistoryDays: {   // по предустановленным значениям; применяется при выходе из меню
        int8_t i = 0;
        while (i < HISTORY_DAYS_STEPS_COUNT - 1 && HISTORY_DAYS_STEPS[i] < s_histDays) i++;
        i = (int8_t)clampI(i + dir, 0, HISTORY_DAYS_STEPS_COUNT - 1);
        s_histDays = HISTORY_DAYS_STEPS[i];
        break;
    }
    case Item::NetInfo:
    case Item::RadioInfo:
    case Item::About:
    case Item::SysState:
        break;
    case Item::Tasks: {        // страницы по кругу (ES: «-» / «+»; TD: в пункте КН IO14 / КН Boot)
        const uint8_t pages = taskPages();
        s_taskPage = (uint8_t)((s_taskPage + pages + dir) % pages);
        break;
    }
    case Item::TouchCal:   // двойное «+»: стереть калибровку и перезапуститься — калибровка при старте
        if (!HAS_TOUCH || dir < 0) { s_resetArmed = false; break; }
        if (!s_resetArmed)         { s_resetArmed = true; break; }
        exitMenu();               // сохранить изменённые настройки
        Cfg::clearTouchCal();
        ESP.restart();
        break;
    case Item::WifiMode:       // выкл / вкл / часы по кругу; применяется сразу (Net перезапускает Wi-Fi), в NVS — при выходе
        Cfg::net.wifiMode = (uint8_t)((Cfg::net.wifiMode + WIFI_MODE_COUNT + dir) % WIFI_MODE_COUNT);
        s_dirtyNet = true;
        Net::requestReconnect();
        break;
    case Item::WebPassReset:   // двойное «+»: пароль настроек веба — NET_WEB_PASS_DEF
        if (dir < 0)       { s_resetArmed = false; break; }
        if (!s_resetArmed) { s_resetArmed = true; break; }
        strlcpy(Cfg::net.webPass, NET_WEB_PASS_DEF, sizeof(Cfg::net.webPass));
        s_dirtyNet = true;
        s_resetArmed = false;
        s_webPassDone = true;
        break;
    case Item::FactoryReset:
        if (dir < 0)       { s_resetArmed = false; break; }
        if (!s_resetArmed) { s_resetArmed = true; break; }
        Cfg::restoreDefaults();   // сеть не сбрасывается
        s_fanSel = 0;
        s_resetArmed = false;
        s_resetDone = true;
        s_dirtyCo2 = s_dirtyFans = s_dirtyVars = false;   // уже сохранено
        s_recalc = true;
        break;
    default: {
        uint16_t& t = Cfg::co2.thresholds[(uint8_t)it - (uint8_t)Item::Threshold1];
        t = (uint16_t)clampI((int16_t)t + dir * PPM_STEP, 0, INT16_MAX);
        s_dirtyCo2 = s_recalc = true;
        break;
    }
    }
}

// Кнопки (тач) / подсказка (кнопки): в списке групп — «Выход», в группе — «Назад»
static void drawMenuStatic()
{
    lcd.fillScreen(COL_BG);
#if HAS_TOUCH
    drawButton(lcd.width() - EXIT_BTN_W, 0, EXIT_BTN_W, HEADER_H + MARGIN * 2, s_inGroup ? "Назад" : "Выход");
    int16_t w4 = lcd.width() / 4, y = lcd.height() - BTN_ROW_H;
    drawButton(0,          y, w4, BTN_ROW_H, "<");
    drawButton(w4,         y, w4, BTN_ROW_H, "-");
    drawButton(w4 * 2,     y, w4, BTN_ROW_H, "+");
    drawButton(w4 * 3,     y, lcd.width() - w4 * 3, BTN_ROW_H, ">");
#else
    // ДН: первое — Boot, второе — IO14 (порядок как у КН)
    const char* hint = !s_inGroup ? "Boot:след IO14:пред ДН:вход/выход"
                     : !s_inItem  ? "Boot:след IO14:пред ДН:изм./назад"
                     : curItem() == Item::Tasks ? "Boot:стр.+ IO14:стр.- ДН IO14:назад"
                                  : "Boot:+  IO14:-  ДН IO14:готово";
    drawLine(hint, lcd.height() - HINT_H, HINT_H, FONT_SMALL, s_inItem ? COL_OK : COL_DIM);
#endif
}

// Карточка меню: заголовок, название параметра (1/3 высоты), значение (остальное)
static int16_t menuTop()    { return HEADER_H + MARGIN * 2; }
static int16_t menuBottom() { return lcd.height() - BTN_ROW_H - HINT_H; }
static int16_t menuValueTop() { return menuTop() + (menuBottom() - menuTop()) / 3; }

// Галочка (связь есть, зелёная) / крестик (нет, красный) толщиной 2 px от x, по центру cy
static void drawStatusIcon(int16_t x, int16_t cy, bool ok)
{
    const int16_t h = STATUS_ICON_H / 2, xm = x + STATUS_ICON_W / 3, x1 = x + STATUS_ICON_W - 1;
    for (int16_t d = 0; d <= 1; d++) {
        if (ok) {
            lcd.drawLine(x, cy + d, xm, cy + h + d, COL_OK);
            lcd.drawLine(xm, cy + h + d, x1, cy - h + d, COL_OK);
        } else {
            lcd.drawLine(x + d, cy - h, x1 - 1 + d, cy + h, COL_ERR);
            lcd.drawLine(x + d, cy + h, x1 - 1 + d, cy - h, COL_ERR);
        }
    }
}

// Строка карточки («Сеть Wi-Fi», «О программе»): текст слева (мельче, если не влезает); status: -1 — без значка,
// 0 — крестик, 1 — галочка (справа от текста)
static void drawInfoLine(const char* text, int16_t y, uint16_t color, int8_t status)
{
    const int32_t maxW = lcd.width() - 2 * MARGIN - (status >= 0 ? STATUS_ICON_W + HDR_GAP : 0);
    char t[INFO_LINE_BYTES];
    strlcpy(t, text, sizeof(t));
    lcd.fillRect(0, y, lcd.width(), NET_LINE_H, COL_BG);
    lcd.setFont(FONT_TEXT);
    if (lcd.textWidth(t) > maxW) lcd.setFont(FONT_SMALL);
    trimToWidth(t, maxW);                        // и мелким не влезает (длинный SSID) — обрезать, значок остаётся виден
    lcd.setTextDatum(lgfx::middle_left);
    lcd.setTextColor(color, COL_BG);
    lcd.drawString(t, MARGIN, y + NET_LINE_H / 2);
    if (status >= 0) drawStatusIcon(MARGIN + lcd.textWidth(t) + HDR_GAP, y + NET_LINE_H / 2, status > 0);
}

// Значение пункта «Сеть Wi-Fi»:
//   Wi-Fi выключен              — выключен в меню (пункт Wi-Fi выше), больше ничего
//   Режим «часы» — 3 строки: «Часы: роутер <SSID>», «Время получено N мин назад» / «Время ещё не получено»,
//   «Синхронизация...» / «Следующая через N мин»
//   AP: CO2-Hub, 192.168.4.1   — только если точка доступа включена (имя и пароль — из настроек веба)
//   Пароль AP: co2sensor
//   STA: <SSID> ✓ / ✗          — связь с роутером есть / нет («сеть не задана», если SSID пуст)
//   IP: <адрес станции>        — только при связи с роутером (тогда точка доступа выключена)
// force = false — перерисовка только при изменении (вызывается раз в UI_REFRESH_MS)
static void drawNetInfo(bool force)
{
    char ap[INFO_LINE_BYTES] = "", pass[INFO_LINE_BYTES] = "", sta[INFO_LINE_BYTES], ip[INFO_LINE_BYTES] = "";
    char a[20], sig[sizeof(s_netShown)];
    const bool on = Net::wifiOn(), en = Net::staEnabled(), ok = Net::staConnected();
    const bool clock = Net::wifiMode() == WIFI_MODE_CLOCK;
    if (clock) {             // режим «Часы»: роутер, когда получено время, когда следующая попытка
        if (Net::staSsid()[0]) snprintf(ap, sizeof(ap), "Часы: роутер %s", Net::staSsid());
        else                   snprintf(ap, sizeof(ap), "Часы: роутер не задан");
        const uint32_t okUp = Net::clockLastOkUpS();
        if (okUp) snprintf(pass, sizeof(pass), "Время получено %lu мин назад", (unsigned long)((Net::uptimeS() - okUp) / S_PER_MIN));
        else      snprintf(pass, sizeof(pass), "Время ещё не получено");
        if (Net::clockWindow()) snprintf(sta, sizeof(sta), "Синхронизация...");
        else snprintf(sta, sizeof(sta), "Следующая через %lu мин", (unsigned long)((Net::clockNextS() + S_PER_MIN - 1) / S_PER_MIN));
    } else if (!on) {
        snprintf(sta, sizeof(sta), "Wi-Fi выключен");
    } else {
        if (Net::apActive()) {   // точка доступа: имя, адрес и пароль (экран — у владельца устройства)
            Net::apIpString(a, sizeof(a));
            snprintf(ap, sizeof(ap), "AP: %s, %s", Net::apSsid(), a);
            snprintf(pass, sizeof(pass), "Пароль AP: %s", Net::apPass());
        }
        if (en) snprintf(sta, sizeof(sta), "STA: %s", Net::staSsid());
        else    snprintf(sta, sizeof(sta), "STA: сеть не задана");
        if (ok && !ap[0]) { Net::staIpString(a, sizeof(a)); snprintf(ip, sizeof(ip), "IP: %s", a); }   // не больше 3 строк
    }
    snprintf(sig, sizeof(sig), "%s|%s|%s|%d|%s", ap, pass, sta, ok ? 1 : 0, ip);
    if (!force && strcmp(sig, s_netShown) == 0) return;
    strlcpy(s_netShown, sig, sizeof(s_netShown));

    const int16_t top = menuValueTop(), bottom = menuBottom();
    const int16_t n = (ap[0] ? 2 : 0) + 1 + (ip[0] ? 1 : 0);
    lcd.fillRect(0, top, lcd.width(), bottom - top, COL_BG);
    int16_t y = top + (bottom - top - n * NET_LINE_H) / 2;
    if (ap[0]) {
        drawInfoLine(ap, y, COL_WARN, -1);   y += NET_LINE_H;
        drawInfoLine(pass, y, COL_WARN, -1); y += NET_LINE_H;
    }
    if (clock) drawInfoLine(sta, y, COL_WARN, -1);
    else       drawInfoLine(sta, y, on && en ? COL_WARN : COL_DIM, on && en ? (ok ? 1 : 0) : -1);
    y += NET_LINE_H;
    if (ip[0]) drawInfoLine(ip, y, COL_WARN, -1);
}

// «О программе»: версия и плата, дата сборки, автор — ABOUT_LINES строк по NET_LINE_H от y
// (меню, п. «О программе», и экран запуска)
constexpr int16_t ABOUT_LINES = 3;
static void drawAboutLines(int16_t y, uint16_t color)
{
    char ver[64], build[48];
    snprintf(ver, sizeof(ver), "Версия %s, %s, %s", FW_VERSION, BOARD_NAME, CO2_SENSOR_NAME);
    snprintf(build, sizeof(build), "Сборка: %s", FW_BUILD_DATE);
    const char* const lines[ABOUT_LINES] = { ver, build, "Автор: " FW_AUTHOR };
    for (int16_t i = 0; i < ABOUT_LINES; i++, y += NET_LINE_H) drawInfoLine(lines[i], y, color, -1);
}

static void drawAbout()
{
    const int16_t top = menuValueTop(), bottom = menuBottom();
    lcd.fillRect(0, top, lcd.width(), bottom - top, COL_BG);
    drawAboutLines(top + (bottom - top - ABOUT_LINES * NET_LINE_H) / 2, COL_WARN);
}

// «Состояние системы»: SYS_LINES строк 9x15 на всю карточку (от menuTop: на T-Display 116 px = 6 x 19 px),
// название — первой строкой (отдельной строки-названия крупным шрифтом нет — иначе на T-Display влезло бы 4):
//   Состояние системы
//   CPU: 12%, 35%, 45.3°C               — загрузка ядер 0 и 1 (статистика задач FreeRTOS), температура чипа
//   Свободно RAM 180 КБ, PSRAM 7.5 МБ
//   Буфер: 7.0 сут (30240), 236 КБ      — вмещает (сутки при текущем периоде, записей), выделено CO2 + t
//   Занято: 4.6 сут (19872)             — записано
//   CO2 116 КБ, t° 39 КБ                — занято записанным (t° — записи с температурой; буфера нет — «t° нет»); ° — DEG_MARK
// В строке 34 символа (320 px / 9 px). Худший случай (PSRAM 8 МБ, 365 сут, ~1 млн записей):
// «Буфер: 365.0 сут (1017290), 8.1 МБ» — 34 символа.
// force = false — перерисовка только при изменении
static void drawSysInfo(bool force)
{
    char l[SYS_LINES][SYS_LINE_BYTES], a[16], b[16], c[16];
    snprintf(l[0], sizeof(l[0]), "Состояние системы");
    if (SysInfo::chipTempValid()) { tenthsText(SysInfo::chipTempTenths(), a, sizeof(a)); }
    else                          { strlcpy(a, "--", sizeof(a)); }
    snprintf(l[1], sizeof(l[1]), "CPU: %u%%, %u%%, %s%cC", SysInfo::cpuLoad(0), SysInfo::cpuLoad(1), a, DEG_MARK);
    bytesText(ESP.getFreeHeap(), a, sizeof(a));
    bytesText(ESP.getFreePsram(), b, sizeof(b));
    snprintf(l[2], sizeof(l[2]), "Свободно RAM %s, PSRAM %s", a, b);
    histDaysText(History::capacity(), a, sizeof(a));
    bytesText(History::co2Bytes() + History::tempBytes(), b, sizeof(b));
    snprintf(l[3], sizeof(l[3]), "Буфер: %s сут (%lu), %s", a, (unsigned long)History::capacity(), b);
    histDaysText(History::count(), a, sizeof(a));
    snprintf(l[4], sizeof(l[4]), "Занято: %s сут (%lu)", a, (unsigned long)History::count());
    bytesText((uint64_t)History::count() * HISTORY_CO2_REC_BYTES, a, sizeof(a));
    if (History::hasTemp()) {
        bytesText((uint64_t)History::tempCount() * HISTORY_TEMP_REC_BYTES, b, sizeof(b));
        snprintf(l[5], sizeof(l[5]), "CO2 %s, t%c %s", a, DEG_MARK, b);
    } else {
        snprintf(l[5], sizeof(l[5]), "CO2 %s, t%c нет", a, DEG_MARK);
    }
    char sig[sizeof(s_sysShown)];
    size_t n = 0;
    for (uint8_t i = 1; i < SYS_LINES && n < sizeof(sig); i++) n += snprintf(sig + n, sizeof(sig) - n, "%s|", l[i]);
    if (!force && strcmp(sig, s_sysShown) == 0) return;
    strlcpy(s_sysShown, sig, sizeof(s_sysShown));

    const int16_t top = menuTop(), bottom = menuBottom();
    lcd.fillRect(0, top, lcd.width(), bottom - top, COL_BG);
    lcd.setFont(FONT_SMALL);
    lcd.setTextDatum(lgfx::middle_left);
    int16_t y = top + (bottom - top - SYS_LINES * SYS_LINE_H) / 2;
    for (uint8_t i = 0; i < SYS_LINES; i++, y += SYS_LINE_H) {
        const uint16_t col = i == 0 ? COL_TEXT : COL_WARN;
        lcd.setTextColor(col, COL_BG);
        drawStrDeg(l[i], MARGIN, y + SYS_LINE_H / 2, col);
    }
}

// «Задачи FreeRTOS»: таблица на всю карточку (от menuTop), строки 9x15 по TASK_LINE_H:
//   Задача       сост  П  стек итог 1/3   — заголовок, страница справа
//   loopTask     раб   1  5432  12%       — имя, состояние, приоритет, мин. свободный стек за всё время (байт),
//                                           доля времени ЦП с запуска (оба ядра; <1% — меньше процента)
// Стек меньше TASK_STACK_WARN_B — красным. Порядок — SysInfo (по убыванию приоритета, затем по имени).
// force = false — перерисовка только по новому снимку SysInfo (раз в SYSINFO_PERIOD_MS)
static uint8_t taskRowsPerPage() { return (uint8_t)((menuBottom() - menuTop()) / TASK_LINE_H - 1); }

static uint8_t taskPages()
{
    const uint8_t per = taskRowsPerPage(), n = SysInfo::taskCount();
    return n ? (uint8_t)((n + per - 1) / per) : 1;
}

// Ячейка таблицы: текст от колонки col (left) или правым краем у колонки col (right)
static void taskCell(const char* s, uint8_t col, bool right, int16_t cy, int16_t charW)
{
    lcd.setTextDatum(right ? lgfx::middle_right : lgfx::middle_left);
    lcd.drawString(s, MARGIN + col * charW, cy);
}

static void drawTasks(bool force)
{
    if (!force && SysInfo::snapshotId() == s_taskShown) return;
    s_taskShown = SysInfo::snapshotId();
    const uint8_t per = taskRowsPerPage(), pages = taskPages(), n = SysInfo::taskCount();
    if (s_taskPage >= pages) s_taskPage = pages - 1;
    const int16_t top = menuTop(), bottom = menuBottom();
    lcd.setFont(FONT_SMALL);
    const int16_t cw = (int16_t)lcd.textWidth("0");   // моноширинный: ширина знака
    char buf[SYS_LINE_BYTES];

    // Заголовок
    int16_t cy = top + TASK_LINE_H / 2;
    lcd.fillRect(0, top, lcd.width(), TASK_LINE_H, COL_BG);
    lcd.setTextColor(COL_DIM, COL_BG);
    taskCell("Задача", 0, false, cy, cw);
    taskCell("сост", TASK_COL_STATE, false, cy, cw);
    taskCell("П", TASK_COL_PRIO_E, true, cy, cw);
    taskCell("стек", TASK_COL_STACK_E, true, cy, cw);
    taskCell("итог", TASK_COL_SHARE_E, true, cy, cw);
    snprintf(buf, sizeof(buf), "%u/%u", s_taskPage + 1, pages);
    lcd.setTextColor(COL_HEADER, COL_BG);
    lcd.setTextDatum(lgfx::middle_right);
    lcd.drawString(buf, lcd.width() - MARGIN, cy);

    for (uint8_t r = 0; r < per; r++) {
        const int16_t y = top + (r + 1) * TASK_LINE_H;
        cy = y + TASK_LINE_H / 2;
        lcd.fillRect(0, y, lcd.width(), TASK_LINE_H, COL_BG);
        if (n == 0 && r == 0 && SysInfo::taskOverflow()) {   // массив снимка мал — FreeRTOS не отдаёт ничего
            lcd.setTextColor(COL_ERR, COL_BG);
            snprintf(buf, sizeof(buf), "Задач больше %u", SYSINFO_TASKS_MAX);
            taskCell(buf, 0, false, cy, cw);
            continue;
        }
        const SysInfo::TaskRow* t = SysInfo::task((uint8_t)(s_taskPage * per + r));
        if (!t) continue;
        lcd.setTextColor(COL_WARN, COL_BG);
        strlcpy(buf, t->name, TASK_NAME_COLS + 1);              // имена задач — ASCII
        taskCell(buf, 0, false, cy, cw);
        taskCell(SysInfo::stateShort(t->state), TASK_COL_STATE, false, cy, cw);
        snprintf(buf, sizeof(buf), "%u", t->prio);
        taskCell(buf, TASK_COL_PRIO_E, true, cy, cw);
        if (t->stackMinB < TASK_STACK_WARN_B) lcd.setTextColor(COL_ERR, COL_BG);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)t->stackMinB);
        taskCell(buf, TASK_COL_STACK_E, true, cy, cw);
        lcd.setTextColor(COL_WARN, COL_BG);
        if (t->shareTenths >= TENTHS_PER_PCT) snprintf(buf, sizeof(buf), "%u%%", t->shareTenths / TENTHS_PER_PCT);
        else                                  snprintf(buf, sizeof(buf), "<1%%");
        taskCell(buf, TASK_COL_SHARE_E, true, cy, cw);
    }
    lcd.fillRect(0, top + (per + 1) * TASK_LINE_H, lcd.width(), bottom - top - (per + 1) * TASK_LINE_H, COL_BG);
}

static void drawMenuItem()
{
    char buf[64];
    const Item it = curItem();
    const MenuGroup& g = MENU_GROUPS[s_group];
    if (!s_inGroup)    snprintf(buf, sizeof(buf), "Настройки: группа %u/%u", s_group + 1, GROUP_COUNT);
    else if (isFanItem(it)) snprintf(buf, sizeof(buf), "%u/%u  В%u: %s", s_pos + 1, g.count, s_fanSel + 1, Cfg::fans[s_fanSel].name);
    else               snprintf(buf, sizeof(buf), "%s %u/%u", g.name, s_pos + 1, g.count);
    lcd.fillRect(0, 0, lcd.width() - (HAS_TOUCH ? EXIT_BTN_W : 0), HEADER_H + MARGIN * 2, COL_BG);
    lcd.setFont(FONT_SMALL);
    trimToWidth(buf, lcd.width() - (HAS_TOUCH ? EXIT_BTN_W : 0) - 2 * MARGIN);   // длинное имя вентилятора — не под «Назад»
    lcd.setTextDatum(lgfx::middle_left);
    lcd.setTextColor(COL_HEADER, COL_BG);
    lcd.drawString(buf, MARGIN, (HEADER_H + MARGIN * 2) / 2);

    const int16_t top = menuTop(), valueTop = menuValueTop(), bottom = menuBottom();
    if (!s_inGroup) {   // карточка группы: подсказка + название группы крупно
        snprintf(buf, sizeof(buf), "Параметров: %u", g.count);
        drawFitted(buf, top, valueTop - top, LABEL_FONTS, sizeof(LABEL_FONTS) / sizeof(LABEL_FONTS[0]), COL_DIM);
        drawFitted(g.name, valueTop, bottom - valueTop, VALUE_FONTS, sizeof(VALUE_FONTS) / sizeof(VALUE_FONTS[0]), COL_WARN);
        return;
    }
    if (it == Item::SysState) {   // на всю карточку, название — первой строкой
        drawSysInfo(true);
        return;
    }
    if (it == Item::Tasks) {      // таблица на всю карточку
        drawTasks(true);
        return;
    }
    itemLabel(it, buf, sizeof(buf));
    drawFitted(buf, top, valueTop - top, LABEL_FONTS, sizeof(LABEL_FONTS) / sizeof(LABEL_FONTS[0]), COL_TEXT);
    if (it == Item::NetInfo) {
        drawNetInfo(true);
        return;
    }
    if (it == Item::About) {
        drawAbout();
        return;
    }
    itemValue(it, buf, sizeof(buf));
    drawFitted(buf, valueTop, bottom - valueTop, VALUE_FONTS, sizeof(VALUE_FONTS) / sizeof(VALUE_FONTS[0]), s_inItem ? COL_OK : COL_WARN);   // TD: редактирование — зелёным
}

static void enterMenu()
{
    s_screen = Screen::Menu;
    s_inGroup = false;            // с карточки последней открытой группы
    s_inItem = false;
    s_resetArmed = false;
    s_resetDone = false;
    s_webPassDone = false;
    s_histDays = Cfg::co2.historyDays;
    s_menuPeriodS = Cfg::co2.transmitPeriodS;
    drawMenuStatic();
    drawMenuItem();
}

static void exitMenu()
{
    // срок или период рассылки изменились: буферы пересоздаются (записей = сутки / период), история очищается
    if (s_histDays != Cfg::co2.historyDays || Cfg::co2.transmitPeriodS != s_menuPeriodS) {
        if (s_histDays != Cfg::co2.historyDays) s_dirtyCo2 = true;
        Cfg::co2.historyDays = s_histDays;
        History::configure(Cfg::co2.historyDays, Cfg::co2.transmitPeriodS, Cfg::co2.tempSensor != TEMP_SENSOR_NONE);
    }
    Cfg::sanitize();
    if (s_dirtyCo2)  Cfg::saveCo2();
    if (s_dirtyFans) Cfg::saveFans();
    if (s_dirtyVars) Cfg::saveVars();
    if (s_dirtyNet)  Cfg::saveNet();
    if (s_recalc)    FanControl::sendNow(FAN_ALL, true);
    s_dirtyCo2 = s_dirtyFans = s_dirtyVars = s_dirtyNet = s_recalc = false;
    s_screen = Screen::Main;
    drawMainStatic();
}

// =====================================================================================
// Ввод: кнопки и тач
// =====================================================================================
static BtnEvent pollButton(Button& b, uint32_t now)
{
    if (b.pin < 0) return BtnEvent::None;
    bool raw = digitalRead(b.pin) == LOW;
    if (raw != b.raw) { b.raw = raw; b.changeTime = now; }
    if (now - b.changeTime < BTN_DEBOUNCE_MS) return BtnEvent::None;
    if (raw && !b.pressed) {
        b.pressed = true;
        b.longFired = false;
        b.pressTime = now;
    } else if (raw && !b.longFired && now - b.pressTime >= BTN_LONG_MS) {
        b.longFired = true;
        return BtnEvent::Long;
    } else if (!raw && b.pressed) {
        b.pressed = false;
        if (!b.longFired) return BtnEvent::Short;
    }
    return BtnEvent::None;
}

// Кнопки: A — BOOT (у T-Display сверху), B — IO14 (снизу); КН — коротко, ДН — долго (BTN_LONG_MS).
//   Главный:  ДН Boot — меню; КН Boot — график; КН IO14 — режим скорости.
//             В режиме скорости: КН Boot «+», КН IO14 «-», ДН IO14 — выход из режима, ДН Boot — меню.
//   График:   КН IO14 — к старым, КН Boot — к новым, ДН Boot — масштаб (по кругу), ДН IO14 — выход.
//   Меню:     КН Boot — след. группа / пункт, в редактировании «+»; КН IO14 — пред. / «-»;
//             ДН Boot — войти (в группу / в редактирование пункта); ДН IO14 — назад (из пункта / группы / меню).
// ES3C28P: только BOOT (B нет), основное управление — тач: главный — ДН меню; график — КН к старым, ДН выход;
// меню — КН следующий, ДН назад / выход (как раньше).
static Key buttonsToKey(uint32_t now)
{
    constexpr bool TWO_BUTTONS = BTN_PIN_B >= 0;
    BtnEvent a = pollButton(s_btnA, now);
    BtnEvent b = pollButton(s_btnB, now);
    if (s_screen == Screen::Main) {
        if (a == BtnEvent::Long)  return Key::Menu;
        if (a == BtnEvent::Short) return !TWO_BUTTONS ? Key::None : s_btnShown ? Key::Inc : Key::History;
        if (b == BtnEvent::Short) return s_btnShown ? Key::Dec : Key::Adjust;
        if (b == BtnEvent::Long)  return s_btnShown ? Key::Hide : Key::None;
    } else if (s_screen == Screen::History) {
        if (b == BtnEvent::Short) return Key::Prev;    // к старым
        if (a == BtnEvent::Short) return TWO_BUTTONS ? Key::Next : Key::Prev;   // к новым (ES: к старым, как раньше)
        if (a == BtnEvent::Long)  return TWO_BUTTONS ? Key::Menu : Key::Exit;   // масштаб по кругу (ES: выход)
        if (b == BtnEvent::Long)  return Key::Exit;
    } else {
        if (a == BtnEvent::Short) return s_inItem ? Key::Inc : Key::Next;
        if (b == BtnEvent::Short) return s_inItem ? Key::Dec : Key::Prev;
        if (a == BtnEvent::Long)  return TWO_BUTTONS ? Key::Enter : Key::Exit;   // ES: BOOT долго — назад / выход
        if (b == BtnEvent::Long)  return Key::Exit;
    }
    return Key::None;
}

#if HAS_TOUCH
static Key touchZone(int32_t x, int32_t y)
{
    const int32_t w = lcd.width();
    if (s_screen == Screen::Main) {   // кнопки — над нижней строкой; касание в другом месте — показать кнопки
        if (s_btnShown && y >= mainBtnY() && y < mainBtnY() + BTN_ROW_H) {
            switch (x / mainBtnW()) {
            case 0:  return Key::Inc;
            case 1:  return Key::Menu;
            case 2:  return Key::History;
            case 3:  return Key::Dec;
            default: return Key::Hide;
            }
        }
        return Key::ShowButtons;
    }
    if (y >= lcd.height() - BTN_ROW_H) {
        if (s_screen == Screen::History) {
            const int32_t sb = histSideBtnW();
            if (x < sb)                  return Key::Prev;
            if (x < 2 * sb)              return Key::Dec;
            if (x < 2 * sb + EXIT_BTN_W) return Key::Exit;
            if (x < 3 * sb + EXIT_BTN_W) return Key::Inc;
            return Key::Next;
        }
        switch (x / (w / 4)) {
        case 0:  return Key::Prev;
        case 1:  return Key::Dec;
        case 2:  return Key::Inc;
        default: return Key::Next;
        }
    }
    if (s_screen == Screen::Menu && y < HEADER_H + MARGIN * 2 && x >= w - EXIT_BTN_W) return Key::Exit;
    if (s_screen == Screen::Menu && !s_inGroup && y >= menuTop()) return Key::Inc;   // касание карточки группы — открыть
    return Key::None;                                      // на графике — свайп
}
#endif

static Key touchToKey(uint32_t now)
{
#if HAS_TOUCH
    if (now - s_touch.lastPoll < TOUCH_POLL_MS) return Key::None;
    s_touch.lastPoll = now;
    lgfx::touch_point_t tp;
    if (lcd.getTouch(&tp, 1) == 0) {
        if (s_touch.down && s_touch.drag && !s_touch.moved) {   // касание графика без сдвига — курсор
            s_lastInput = now;
            histTap(s_touch.startX);
        }
        s_touch.down = false;
        s_touch.drag = false;
        return Key::None;
    }
    if (!s_touch.down) {
        s_touch.down = true;
        s_touch.key = touchZone(tp.x, tp.y);
        s_touch.downTime = s_touch.lastRepeat = now;
        s_touch.drag = s_screen == Screen::History && s_touch.key == Key::None;
        if (UI_TOUCH_LOG) Serial.printf("touch x=%d y=%d key=%u\n", (int)tp.x, (int)tp.y, (unsigned)s_touch.key);
        s_touch.lastX = s_touch.startX = tp.x;
        s_touch.moved = false;
        // автоповтор «-»/«+» — кроме коррекции скорости вентилятора и «+» в списке групп (он открывает группу);
        // решается до обработки нажатия: после открытия группы удержание не должно менять значение
        s_touch.repeatOk = s_screen == Screen::History
                         ? (s_touch.key == Key::Prev || s_touch.key == Key::Next)
                         : (s_touch.key == Key::Dec || s_touch.key == Key::Inc) &&
                           !(s_screen == Screen::Menu && (!s_inGroup || curItem() == Item::FanSpeed));
        return s_touch.key;
    }
    if (s_touch.drag) {   // свайп: палец вправо — к более старым данным
        if (!s_touch.moved) {
            if (tp.x - s_touch.startX <= HIST_TAP_MAX_PX && s_touch.startX - tp.x <= HIST_TAP_MAX_PX) return Key::None;
            s_touch.moved = true;      // это свайп, а не выбор точки
            s_histCursor = -1;
        }
        int32_t dx = tp.x - s_touch.lastX;
        if ((dx >= HIST_DRAG_MIN_PX || dx <= -HIST_DRAG_MIN_PX) && now - s_touch.lastDragDraw >= HIST_DRAG_REDRAW_MS) {
            histScroll(dx * HIST_SCALES[s_histScale]);
            s_touch.lastX = tp.x;
            s_touch.lastDragDraw = now;
            s_lastInput = now;
            drawHistoryChart();
        }
        return Key::None;
    }
    if (s_touch.repeatOk && now - s_touch.downTime >= TOUCH_REPEAT_DELAY_MS && now - s_touch.lastRepeat >= TOUCH_REPEAT_MS) {
        s_touch.lastRepeat = now;
        return s_touch.key;
    }
#else
    (void)now;
#endif
    return Key::None;
}

static void handleKey(Key k)
{
    if (s_screen == Screen::Main) {
        switch (k) {
        case Key::Dec:
        case Key::Inc: {   // общая поправка — только при активной панели (ES — кнопки, TD — режим скорости)
            if (!s_btnShown) break;
            int8_t d = k == Key::Inc ? 1 : -1;
            Cfg::vars.speedOverride = (int8_t)clampI(Cfg::vars.speedOverride + d, -FAN_SPEED_MAX, FAN_SPEED_MAX);
            FanControl::sendNow(FAN_ALL, true);   // в RAM, как форма на главной странице хаба
            break;
        }
        case Key::ShowButtons:
        case Key::Adjust:      if (!s_btnShown) showMainButtons(true); break;
        case Key::Hide:        if (s_btnShown) showMainButtons(false); break;
        case Key::Menu:        s_btnShown = false; enterMenu(); break;
        case Key::History:     s_btnShown = false; enterHistory(); break;
        default: break;
        }
        return;
    }
    if (s_screen == Screen::History) {
        handleHistoryKey(k);
        return;
    }
    if (!s_inGroup) {   // список групп
        switch (k) {
        case Key::Prev:  s_group = s_group == 0 ? GROUP_COUNT - 1 : s_group - 1; break;
        case Key::Next:  s_group = (uint8_t)((s_group + 1) % GROUP_COUNT); break;
        case Key::Inc:                                      // ES: «+» / касание карточки
        case Key::Enter: s_inGroup = true; s_pos = 0; s_resetArmed = false; drawMenuStatic(); break;   // TD: ДН Boot
        case Key::Exit:  exitMenu(); return;
        default: return;
        }
        drawMenuItem();
        return;
    }
    if (s_inItem) {     // TD: редактирование пункта
        switch (k) {
        case Key::Dec:  changeItem(curItem(), -1); break;
        case Key::Inc:  changeItem(curItem(), +1); break;
        case Key::Exit: s_inItem = false; s_resetArmed = false; drawMenuStatic(); break;   // ДН IO14 — к пунктам
        default: return;
        }
        drawMenuItem();
        return;
    }
    const uint8_t n = MENU_GROUPS[s_group].count;
    switch (k) {
    case Key::Prev:  s_pos = s_pos == 0 ? n - 1 : s_pos - 1; s_resetArmed = false; break;
    case Key::Next:  s_pos = (uint8_t)((s_pos + 1) % n);     s_resetArmed = false; break;
    case Key::Dec:   changeItem(curItem(), -1); break;      // ES: «-» / «+» сразу меняют значение
    case Key::Inc:   changeItem(curItem(), +1); break;
    case Key::Enter:                                         // TD: ДН Boot — редактирование (у карточек-сведений нет)
        if (isInfoItem(curItem())) return;
        s_inItem = true; drawMenuStatic(); break;
    case Key::Exit:  s_inGroup = false; s_resetArmed = false; drawMenuStatic(); break;   // назад к группам
    default: return;
    }
    drawMenuItem();
}

// =====================================================================================
// Калибровка тачскрина: при первом запуске (нет данных в NVS) или после «Калибровка тача»
// в меню / на веб-странице. Только из setup(): calibrateTouch() ждёт касаний (блокирующая).
// =====================================================================================
#if HAS_TOUCH
static bool touchChipPresent()
{
    return lgfx::i2c::readRegister8(I2C_PORT, TOUCH_I2C_ADDR, TOUCH_REG_VENDOR_ID, K22_I2C_HZ).has_value();
}

static void touchCalibrate()
{
    uint16_t cal[Cfg::TOUCH_CAL_LEN];
    if (Cfg::loadTouchCal(cal)) {
        lcd.setTouchCalibrate(cal);
        Serial.println("Тач: калибровка загружена из NVS");
        return;
    }
    if (!touchChipPresent()) {   // без чипа calibrateTouch() ждала бы вечно
        Serial.println("Тач: FT6336G не отвечает — калибровка пропущена");
        return;
    }
    const int16_t cx = lcd.width() / 2, cy = lcd.height() / 2;
    lcd.fillScreen(COL_BG);
    drawLine("Калибровка тачскрина", cy - 2 * INFO_LINE_H, INFO_LINE_H, FONT_TEXT, COL_HEADER, lgfx::middle_center, cx);
    drawLine("Нажмите по очереди на", cy - INFO_LINE_H / 2, INFO_LINE_H, FONT_TEXT, COL_TEXT, lgfx::middle_center, cx);
    drawLine("отметки в углах экрана", cy + INFO_LINE_H, INFO_LINE_H, FONT_TEXT, COL_TEXT, lgfx::middle_center, cx);
    lcd.calibrateTouch(cal, COL_WARN, COL_BG, TOUCH_CAL_MARK_SIZE);
    bool saved = Cfg::saveTouchCal(cal);
    Serial.printf("Тач: калибровка %u %u %u %u %u %u %u %u -> NVS %s\n",
                  cal[0], cal[1], cal[2], cal[3], cal[4], cal[5], cal[6], cal[7], saved ? "OK" : "ОШИБКА");
    lcd.fillScreen(COL_BG);
    drawLine(saved ? "Калибровка сохранена" : "Ошибка записи NVS", cy - INFO_LINE_H / 2, INFO_LINE_H,
             FONT_TEXT, saved ? COL_OK : COL_ERR, lgfx::middle_center, cx);
    delay(TOUCH_CAL_DONE_MS);   // вызывается только из setup()
    lcd.fillScreen(COL_BG);
}
#endif

// =====================================================================================
void begin()
{
#if defined(BOARD_TDISPLAY_S3)
    pinMode(LCD_PIN_POWER_ON, OUTPUT);
    digitalWrite(LCD_PIN_POWER_ON, HIGH);
#endif
    if (BTN_PIN_A >= 0) pinMode(BTN_PIN_A, INPUT_PULLUP);
    if (BTN_PIN_B >= 0) pinMode(BTN_PIN_B, INPUT_PULLUP);
    lcd.init();
    lcd.setRotation(LCD_ROTATION);
    lcd.setBrightness((uint16_t)Cfg::co2.brightDayPct * LCD_BL_MAX / BRIGHT_MAX_PCT);   // до первого update()
    lcd.fillScreen(COL_BG);
#if HAS_TOUCH
    touchCalibrate();
#endif
    drawLine("CO2 датчик: запуск...", 0, HEADER_H, FONT_TEXT, COL_HEADER);
    drawAboutLines(lcd.height() - ABOUT_LINES * NET_LINE_H, COL_TEXT);   // внизу, под отчётом nRF24
}

void setRadioRegsOk(bool ok) { s_radioRegsOk = ok; }

void setRadioReport(bool ok, const char* report)
{
    s_radioRegsOk = ok;
    drawLine(report, HEADER_H, INFO_LINE_H * 3, FONT_SMALL, ok ? COL_OK : COL_ERR);
}

// Яркость подсветки: днём / ночью (ночной режим — расписание или ручной переключатель, как у вентиляторов).
// В меню на пунктах яркости — сразу редактируемое значение (предпросмотр).
static void applyBrightness()
{
    uint8_t pct;
    if (s_screen == Screen::Menu && curItem() == Item::BrightDay)        pct = Cfg::co2.brightDayPct;
    else if (s_screen == Screen::Menu && curItem() == Item::BrightNight) pct = Cfg::co2.brightNightPct;
    else pct = FanControl::nightEffective() ? Cfg::co2.brightNightPct : Cfg::co2.brightDayPct;
    uint8_t v = (uint8_t)((uint16_t)pct * LCD_BL_MAX / BRIGHT_MAX_PCT);
    if (v != s_brightShown) {
        s_brightShown = v;
        lcd.setBrightness(v);
    }
}

void update(uint32_t now)
{
    applyBrightness();
    if (s_lastDraw == 0) {   // первый вызов — главный экран
        s_lastDraw = now;
        drawMainStatic();
    }
    Key k = touchToKey(now);
    Key b = buttonsToKey(now);   // кнопки опрашиваются всегда (антидребезг)
    if (k == Key::None) k = b;
    if (k != Key::None) {
        s_lastInput = now;
        handleKey(k);
    }
    if (s_screen == Screen::Menu) {
        if (now - s_lastInput >= UI_SETUP_TIMEOUT_MS) { exitMenu(); return; }
        if (curItem() == Item::NetInfo && now - s_lastDraw >= UI_REFRESH_MS) {   // состояние сети — вживую
            s_lastDraw = now;
            drawNetInfo(false);
        }
        if (curItem() == Item::SysState && now - s_lastDraw >= UI_REFRESH_MS) {  // состояние системы — вживую
            s_lastDraw = now;
            drawSysInfo(false);
        }
        if (curItem() == Item::Tasks && s_inGroup) drawTasks(false);   // задачи — по новому снимку SysInfo
        return;
    }
    if (s_screen == Screen::History) {
        if (now - s_lastInput >= UI_HISTORY_TIMEOUT_MS) { exitHistory(); return; }
        uint32_t n = History::count();
        if (n != s_histCount) {   // новая запись: при просмотре архива окно остаётся на месте
            if (s_histOffset > 0 && n > s_histCount) s_histOffset += n - s_histCount;
            drawHistoryChart();
        }
        return;
    }
    if (s_btnShown && now - s_lastInput >= UI_MAIN_BUTTONS_MS) showMainButtons(false);
    if (now - s_lastDraw >= UI_REFRESH_MS || k != Key::None) {
        s_lastDraw = now;
        drawMainDynamic(now);
    }
}

} // namespace Ui
