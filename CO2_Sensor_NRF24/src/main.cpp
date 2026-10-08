// main.cpp — датчик CO2 + хаб вентиляторов: SenseAir K22-OC (I2C) или S8 LP (UART) -> экран, веб, nRF24L01+ -> вентиляторы.
// Заменяет пару «датчик AVR + хаб ESP8266» (Old/). Описание — Readme.md.
//
// Fuses: нет. ESP32-S3: eFuse не программируются; Flash/PSRAM задаются в platformio.ini.
//
// loop() неблокирующий: датчик CO2, рассылка и интерфейс — конечные автоматы на millis(); в конце
// прохода — уступка ядра планировщику на LOOP_YIELD_TICKS (1 тик = 1 мс), не блокирующая задержка.
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

constexpr uint32_t STARTUP_REPORT_MS = 3000;   // показ экрана запуска (отчёт радио, версия, дата сборки, автор)
constexpr size_t   REPORT_LEN        = 200;

static char s_report[REPORT_LEN];

void setup()
{
    Serial.begin(SERIAL_BAUD);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < SERIAL_WAIT_MS) delay(10);
    Serial.printf("\n--- CO2 hub %s (%s), плата %s, датчик %s, автор %s ---\n",
                  FW_VERSION, FW_BUILD_DATE, BOARD_NAME, CO2_SENSOR_NAME, FW_AUTHOR);
    Serial.println(FW_ID_MARKER FW_ID);   // единая строка в образе — по ней copy_firmware.py называет файл

    SysInfo::begin();     // задачи FreeRTOS, загрузка ядер, температура кристалла
    Cfg::begin();
    Serial.printf("Настройки: вентиляторов %u, период %u с, Wi-Fi \"%s\"\n",
                  Cfg::co2.fans, Cfg::co2.transmitPeriodS, Cfg::net.ssid);
    Serial.printf("Память: куча %lu байт, PSRAM %lu из %lu байт\n", (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize());
    if (!History::configure(Cfg::co2.historyDays, Cfg::co2.transmitPeriodS, Cfg::co2.tempSensor != TEMP_SENSOR_NONE))
        Serial.println("История: нет памяти!");

    Ui::begin();          // дисплей (+ тач и I2C на ES3C28P)
    Co2Sensor::begin();   // K22: I2C (на ES3C28P — та же шина, что у тача); S8: UART1

    Radio::begin(RF_CHANNEL_DEF);
    bool regsOk = Radio::verifyRegisters(s_report, sizeof(s_report));
    Serial.println(s_report);
    Ui::setRadioReport(regsOk, s_report);
    Web::setRadioReport(s_report);

    FanControl::begin(millis());
    Net::begin();
    if (Net::wifiOn()) Web::begin();   // Wi-Fi выключен — веб запустится при включении (loop)
    delay(STARTUP_REPORT_MS);   // только в setup(): дать прочитать отчёт на экране
}

void loop()
{
    const uint32_t now = millis();
    Net::update(now);
    if (Net::wifiOn()) Web::begin();   // первое включение Wi-Fi из меню — запуск веба (повторно — ничего)
    {
        CfgLock l;
        SysInfo::update(now);   // под замком: таблицу задач читает и веб
        Co2Sensor::update(now);
        TempSensor::update(now, Cfg::co2.tempSensor, Cfg::co2.tempPeriodS);   // датчик температуры: тип из настроек
        History::setTemp(Cfg::co2.tempSensor != TEMP_SENSOR_NONE);           // буфер температуры — при выбранном датчике
        FanControl::update(now, Co2Sensor::co2(), Co2Sensor::isValid(now));
        if (FanControl::radioReconfigured()) {   // смена режима связи: сверка регистров для меню и веба
            const bool regsOk = Radio::verifyRegisters(s_report, sizeof(s_report));
            Serial.println(s_report);
            Ui::setRadioRegsOk(regsOk);
            Web::setRadioReport(s_report);
        }
        Ui::update(now);
    }
    // Уступить ядро планировщику (не блокирующая задержка логики): без этого loop() крутится непрерывно,
    // ядро 1 занято на 100 % вхолостую (греется, загрузка не видна). Вне CfgLock — веб не ждёт.
    vTaskDelay(LOOP_YIELD_TICKS);
}
