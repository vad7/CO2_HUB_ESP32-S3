// main.cpp - датчик CO2 + хаб вентиляторов: SenseAir K22-OC (I2C) или S8 LP (UART) -> экран, веб, nRF24L01+ -> вентиляторы.
// Заменяет пару «датчик AVR + хаб ESP8266» (Old/). Описание - Readme.md.
//
// Fuses: нет. ESP32-S3: eFuse не программируются; Flash/PSRAM задаются в platformio.ini.
//
// loop() неблокирующий: датчик CO2, рассылка и интерфейс - конечные автоматы на millis(); в конце
// прохода - уступка ядра планировщику на LOOP_YIELD_TICKS (1 тик = 1 мс), не блокирующая задержка.
// Веб-сервер работает в своей задаче; общие данные защищены Cfg::lock().
#include <Arduino.h>
#include "config.h"
#include "build_info.h"
#include "hub_config.h"
#include "history.h"
#include "co2_sensor.h"
#include "radio.h"
#include "fan_control.h"
#include "net.h"
#include "web.h"
#include "ui.h"
#include "temp_sensor.h"
#include "sys_info.h"

constexpr uint32_t STARTUP_REPORT_MS = 5000;   // показ экрана запуска (отчёт радио, версия, дата сборки, автор)
constexpr size_t   REPORT_LEN        = 200;

static char s_report[REPORT_LEN];

void setup()
{
    Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS);   // HWCDC: без терминала печать не блокирует
#endif
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < SERIAL_WAIT_MS) delay(10);
    Serial.printf("\n--- CO2 hub %s (%s), плата %s, датчик %s, автор %s ---\n",
                  FW_VERSION, FW_BUILD_DATE, BOARD_NAME, CO2_SENSOR_NAME, FW_AUTHOR);
    Serial.println(FW_ID_MARKER FW_ID);   // единая строка в образе - по ней copy_firmware.py называет файл

    SysInfo::begin();     // задачи FreeRTOS, загрузка ядер, температура кристалла
    Cfg::begin();
    Serial.printf("Настройки: вентиляторов %u, период %u с, Wi-Fi \"%s\"\n",
                  Cfg::co2.fans, Cfg::co2.transmitPeriodS, Cfg::net.ssid);
    Serial.printf("Память: куча %lu б, PSRAM %lu из %lu б\n", (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize());
    if (!History::configure(Cfg::co2.historyDays, Cfg::co2.transmitPeriodS, Cfg::co2.tempSensor != TEMP_SENSOR_NONE))
        Serial.println("История: нет памяти!");

    Ui::begin();          // дисплей (+ тач и I2C на ES3C28P)
    Co2Sensor::begin();   // K22: I2C (на ES3C28P - та же шина, что у тача); S8: UART1

    Radio::begin(RF_CHANNEL_DEF);
    bool regsOk = Radio::verifyRegisters(s_report, sizeof(s_report));
    Serial.println(s_report);
    Ui::setRadioReport(regsOk, s_report);
    Web::setRadioReport(s_report);

    FanControl::begin(millis());
    Net::begin();
    if (Net::wifiOn()) Web::begin();   // Wi-Fi выключен - веб запустится при включении (loop)
    delay(STARTUP_REPORT_MS);   // только в setup(): дать прочитать отчёт на экране
}

// ---------------- Замер прохода loop() (при включённой отладке, Cfg::co2.debug) ----------------
// Максимум каждой части и всего прохода за LOOP_STAT_PERIOD_MS; в Serial - если проход дольше LOOP_SLOW_MS.
// «замок» - ожидание CfgLock (держит веб), «пауза» - vTaskDelay в конце (больше 1 мс - ядро занято другими).
enum LoopPart : uint8_t { LP_NET, LP_LOCK, LP_SYS, LP_CO2, LP_TEMP, LP_FAN, LP_UI, LP_YIELD, LP_COUNT };
static const char* const LOOP_PART_NAMES[LP_COUNT] = { "сеть", "замок", "sys", "co2", "t", "вент", "экран", "пауза" };
constexpr uint32_t US_PER_MS = 1000;
static uint32_t s_partMaxUs[LP_COUNT];
static uint32_t s_passMaxUs = 0;
static uint32_t s_markUs    = 0;
static uint32_t s_statT0    = 0;

static void loopMark(LoopPart p)
{
    const uint32_t t = micros();
    if (t - s_markUs > s_partMaxUs[p]) s_partMaxUs[p] = t - s_markUs;
    s_markUs = t;
}

static void loopStat(uint32_t passStartUs, uint32_t now)
{
    const uint32_t pass = micros() - passStartUs;
    if (pass > s_passMaxUs) s_passMaxUs = pass;
    if (now - s_statT0 < LOOP_STAT_PERIOD_MS) return;
    s_statT0 = now;
    if (s_passMaxUs >= LOOP_SLOW_MS * US_PER_MS) {
        Serial.printf("loop: проход до %lu мс; макс., мс:", (unsigned long)(s_passMaxUs / US_PER_MS));
        for (uint8_t i = 0; i < LP_COUNT; i++)
            Serial.printf(" %s %lu", LOOP_PART_NAMES[i], (unsigned long)(s_partMaxUs[i] / US_PER_MS));
        Serial.println();
    }
    s_passMaxUs = 0;
    for (uint8_t i = 0; i < LP_COUNT; i++) s_partMaxUs[i] = 0;
}

void loop()
{
    const uint32_t now = millis();
    bool debug;
    const uint32_t passStartUs = s_markUs = micros();
    Net::update(now);
    if (Net::wifiOn()) Web::begin();   // первое включение Wi-Fi из меню - запуск веба (повторно - ничего)
    loopMark(LP_NET);
    {
        CfgLock l;
        loopMark(LP_LOCK);
        SysInfo::update(now);   // под замком: таблицу задач читает и веб
        loopMark(LP_SYS);
        Co2Sensor::update(now, Cfg::co2.co2PollS);   // период опроса - из настроек
        loopMark(LP_CO2);
        TempSensor::update(now, Cfg::co2.tempSensor, Cfg::co2.tempPeriodS);   // датчик температуры: тип из настроек
        History::setTemp(Cfg::co2.tempSensor != TEMP_SENSOR_NONE);           // буфер температуры - при выбранном датчике
        loopMark(LP_TEMP);
        FanControl::update(now, Co2Sensor::co2(), Co2Sensor::isValid(now));
        if (FanControl::radioReconfigured()) {   // смена режима связи: сверка регистров для меню и веба
            const bool regsOk = Radio::verifyRegisters(s_report, sizeof(s_report));
            Serial.println(s_report);
            Ui::setRadioRegsOk(regsOk);
            Web::setRadioReport(s_report);
        }
        loopMark(LP_FAN);
        Ui::update(now);
        loopMark(LP_UI);
        debug = Cfg::co2.debug;
    }
    // Уступить ядро планировщику (не блокирующая задержка логики): без этого loop() крутится непрерывно,
    // ядро 1 занято на 100 % вхолостую (греется, загрузка не видна). Вне CfgLock - веб не ждёт.
    vTaskDelay(LOOP_YIELD_TICKS);
    loopMark(LP_YIELD);
    if (debug) loopStat(passStartUs, now);
}
