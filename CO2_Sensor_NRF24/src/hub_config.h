// hub_config.h - настройки хаба (перенос CFG_CO2 / CFG_FAN / GLOBAL_VARS из Old/ESP8266_WIFI/app/wireless_co2.h)
// Хранение - NVS (Preferences). Доступ из loop() и из задачи веб-сервера - под Cfg::lock().
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

// Режим коррекции скорости вентилятора днём/ночью (override_day / override_night)
enum FanOverrideMode : uint8_t {
    OVR_NONE = 0,   // нет
    OVR_SET  = 1,   // = speed
    OVR_ADD  = 2,   // + speed
    OVR_MAX  = 3,   // не больше speed (<=)
    OVR_MIN  = 4,   // не меньше speed (>=)
    OVR_COUNT
};

// Флаги вентилятора (CFG_FAN_FLAGS)
enum FanFlagBits : uint8_t {
    FAN_SKIP_BIT         = 0,   // не передавать
    FAN_SPEED_FORCED_BIT = 1,   // принудительная скорость (speed_current задан вручную)
};
constexpr uint8_t FAN_FLAG_SKIP   = 1 << FAN_SKIP_BIT;
constexpr uint8_t FAN_FLAG_FORCED = 1 << FAN_SPEED_FORCED_BIT;

// Режим связи с вентиляторами
enum RadioMode : uint8_t {
    RADIO_ACTIVE  = 0,   // хаб сам передаёт каждому вентилятору (Old/ESP8266_WIFI)
    RADIO_PASSIVE = 1,   // хаб слушает, вентиляторы запрашивают, ответ - в ACK (Old/CO2UART)
    RADIO_MODE_COUNT
};

// Датчик температуры (выбор в настройках)
enum TempSensorType : uint8_t { TEMP_SENSOR_NONE = 0, TEMP_SENSOR_DS18B20 = 1, TEMP_SENSOR_SHT40 = 2, TEMP_SENSOR_COUNT };

// Шрифт крупных цифр CO2 на главном экране (выбор в настройках)
enum DigitsFont : uint8_t { DIGITS_FONT_7SEG = 0, DIGITS_FONT_SMOOTH = 1, DIGITS_FONT_COUNT };   // Font7 x2 / Montserrat SemiBold

// Режим Wi-Fi (меню экрана): выкл; вкл (станция / точка доступа, веб, NTP); часы - Wi-Fi только для NTP
enum WifiMode : uint8_t { WIFI_MODE_OFF = 0, WIFI_MODE_ON = 1, WIFI_MODE_CLOCK = 2, WIFI_MODE_COUNT };

// Ночь вручную (now_night_override)
enum NightOverride : uint8_t { NIGHT_AUTO = 0, NIGHT_FORCE_OFF = 1, NIGHT_FORCE_ON = 2, NIGHT_OVR_COUNT };

// ---------------------------------------------------------------------------------------------
// ПРАВИЛО СОВМЕСТИМОСТИ НАСТРОЕК (чтобы обновление прошивки не сбрасывало сохранённое):
//   * новые поля добавлять ТОЛЬКО В КОНЕЦ структуры - перед маркером _end;
//   * существующие поля не удалять, не переставлять, не менять тип и размер массива
//     (ненужное поле - оставить и не использовать);
//   * значение по умолчанию нового поля задать в defaults*() / initFan().
//   * если поле всё же удалено или изменено в середине - сменить ключ NVS этой структуры (KEY_* в
//     hub_config.cpp): старый блок не прочитается (иначе поля сдвинутся), структура - по умолчанию;
//   * смена формата самого блока (заголовок BlobHdr) - сменить BLOB_MAGIC: по умолчанию станут ВСЕ структуры.
//     2026-10-07: заголовок 8 -> 6 байт (убран reserved), BLOB_MAGIC 0xC5A1 -> 0xC5A2 - заодно с удалением
//     CfgCo2.historyRecords и именем вентилятора 16 -> 32 байта: все настройки, кроме калибровки тача, сброшены.
// Загрузка (hub_config.cpp): сначала умолчания, затем поверх - min(размер в прошивке, размер в NVS)
// байт каждого элемента. Размер данных - offsetof(T, _end), а не sizeof(T): хвостовое
// выравнивание sizeof не должно попадать в NVS, иначе новое поле, легшее в него, затрётся мусором.
// ---------------------------------------------------------------------------------------------
#define CFG_END_MARKER  uint8_t _end[0]       // маркер конца данных (не данные, размер 0)
#define CFG_DATA_SIZE(T) offsetof(T, _end)

struct CfgCo2 {
    uint16_t thresholds[FAN_SPEED_MAX];   // пороги скорости, ppm
    uint16_t speedDelta;                  // гистерезис понижения скорости, ppm
    uint16_t nightStart;                  // hhmm, будни
    uint16_t nightEnd;
    uint16_t nightStartWd;                // hhmm, выходные (сб, вс)
    uint16_t nightEndWd;
    uint8_t  fans;                        // количество вентиляторов
    uint8_t  nightMaxSpeed;               // макс. скорость ночью
    char     csvDelimiterUnused;          // был разделитель history.csv (с 2026-10-09 - всегда HISTORY_CSV_SEP); место в NVS
    uint16_t pageRefreshMs;               // период обновления веб-страницы
    uint16_t transmitPeriodS;             // период рассылки вентиляторам (новое: у хаба - по приходу пакета датчика)
    uint8_t  brightDayPct;                // яркость подсветки экрана днём, % (0 в NVS = не задано)
    uint8_t  brightNightPct;              // яркость подсветки ночью (по ночному режиму), %
    uint8_t  radioMode;                   // RadioMode: 0 - активный (хаб передаёт), 1 - пассивный (вентиляторы запрашивают)
    uint8_t  passiveChannel;              // пассивный режим: общий радиоканал всех вентиляторов
    uint16_t radioResetS;                 // пассивный режим: нет приёма дольше - перезапуск nRF24, с (0 - нет)
    uint8_t  tempPeriodS;                 // период чтения датчика температуры, с (0 в NVS = не задано)
    uint8_t  tempSensor;                  // TempSensorType: 0 - нет, 1 - DS18B20, 2 - SHT40
    uint16_t historyDays;                 // размер истории, сутки (HISTORY_DAYS_MIN..MAX)
    uint8_t  digitsFont;                  // DigitsFont: 0 - 7-сегментные, 1 - сглаженные (Montserrat)
    uint8_t  debug;                       // 1 - отладка: диагностика в Serial (касания, замер loop()); по умолч. 0
    uint8_t  co2PollS;                    // период опроса датчика CO2, с (0 в NVS = не задано)
    // новые поля - сюда
    CFG_END_MARKER;
};

struct CfgFan {
    char    name[FAN_NAME_LEN + 1];
    uint8_t rfChannel;
    uint8_t addressLsb;
    int8_t  speedMin;
    int8_t  speedMax;
    uint8_t overrideDay;                  // FanOverrideMode
    int8_t  speedDay;
    uint8_t overrideNight;
    int8_t  speedNight;
    uint8_t flags;                        // FAN_FLAG_*
    uint8_t pauseS;                       // пассивный режим: пауза вентилятора до следующего запроса, с (в ACK)
    uint8_t timeoutS;                     // пассивный режим: молчит дольше - «нет связи», с (0 - не проверять)
    // новые поля - сюда
    CFG_END_MARKER;
};

struct GlobalVars {
    int8_t  speedOverride;                // fans_speed_override, -FAN_SPEED_MAX..+FAN_SPEED_MAX
    // новые поля - сюда
    CFG_END_MARKER;
};

struct NetCfg {
    char ssid[33];
    char pass[65];
    char tz[48];                          // POSIX TZ, напр. "MSK-3"
    char ntp[48];
    uint16_t apDelayMin;                  // нет связи с роутером дольше - включить точку доступа (AP+STA)
    uint16_t ntpPeriodMin;                // период обновления времени по NTP, мин (0 в NVS = не задано)
    char     apSsid[33];                  // точка доступа: имя (пусто = по умолчанию), меняется в вебе
    char     apPass[65];                  //   пароль, не короче NET_AP_PASS_MIN
    uint8_t  wifiMode;                    // WifiMode: выкл / вкл / часы (только меню экрана)
    char     webPass[33];                 // пароль настроек веба (HTTP Basic, имя любое); пусто - не запрашивается;
                                          // сброс на NET_WEB_PASS_DEF - меню экрана
    // новые поля - сюда
    CFG_END_MARKER;
};

namespace Cfg {
    extern CfgCo2     co2;
    extern CfgFan     fans[FANS_MAX];
    extern GlobalVars vars;
    extern NetCfg     net;
    extern uint8_t    nightOverride;      // RAM, NightOverride

    void begin();                         // загрузка из NVS или значения по умолчанию
    void lock();                          // рекурсивный мьютекс
    void unlock();
    bool saveCo2();
    bool saveFans();
    bool saveVars();
    bool saveNet();
    void restoreDefaults();               // всё, кроме сети и калибровки тача

    // Калибровка тачскрина (LovyanGFX calibrateTouch: 8 x uint16), отдельный ключ NVS
    constexpr uint8_t TOUCH_CAL_LEN = 8;
    bool loadTouchCal(uint16_t* cal);     // false - калибровки нет
    bool saveTouchCal(const uint16_t* cal);
    void clearTouchCal();                 // при следующем старте - калибровка заново
    void initFan(uint8_t idx);            // умолчания для нового вентилятора
    void sanitize();                      // приведение значений к допустимым диапазонам
}

// RAII-обёртка для Cfg::lock()
struct CfgLock {
    CfgLock()  { Cfg::lock(); }
    ~CfgLock() { Cfg::unlock(); }
    CfgLock(const CfgLock&) = delete;
    CfgLock& operator=(const CfgLock&) = delete;
};
