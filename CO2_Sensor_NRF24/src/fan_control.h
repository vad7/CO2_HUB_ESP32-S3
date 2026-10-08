// fan_control.h — логика хаба (Old/ESP8266_WIFI/app/wireless_co2.c): расчёт скорости по CO2
// с ночным режимом и коррекциями вентиляторов, рассылка по радио.
// Все функции вызываются под Cfg::lock() (loop() и веб-сервер).
#pragma once
#include <stdint.h>
#include "config.h"

// Статус последней передачи — значения NRF24_transmit_status хаба (веб: красный, если > 1)
enum TxStatus : uint8_t { TX_NONE = 0, TX_OK = 1, TX_ERROR = 2, TX_TIMEOUT = 3 };

// Пассивный режим: TX_OK — запрос получен и вентилятор сообщил «ok», TX_ERROR — вентилятор сообщил ошибку
// связи или сбой EEPROM, TX_TIMEOUT — молчит дольше timeoutS, TX_NONE — запросов ещё не было.
struct FanState {
    uint8_t  speedCurrent;     // speed_current
    uint8_t  txStatus;         // transmit_last_status (TxStatus)
    uint32_t txOkUptimeS;      // активный: последняя успешная передача; пассивный: последний запрос (аптайм, с; 0 — не было)
    uint32_t forcedTimeoutS;   // forced_speed_timeout, с (0 — бессрочно)
    uint8_t  remoteStatus;     // пассивный: байт состояния от вентилятора (FAN_ST_*)
    int8_t   remoteAdjust;     // пассивный: поправка скорости на вентиляторе (−8..+7)
    bool     remoteOff;        // пассивный: вентилятор выключен
    bool     remoteEeprom;     // пассивный: вентилятор сообщил о сбое ячейки EEPROM
};

constexpr uint8_t FAN_ALL = 0xFF;

namespace FanControl {
    void     begin(uint32_t now);
    void     update(uint32_t now, uint16_t co2, bool co2Valid);   // из loop()

    void     sendNow(uint8_t fan, bool calcSpeed);   // send_fans_speed_now(); FAN_ALL — все
    // Коррекция с веб-страницы/экрана: 'p' — +1, 'm' — -1 (принудительно на minutes, 0 — бессрочно),
    // 'c' — снять принудительную скорость (cfg_fan_override хаба)
    void     fanOverride(uint8_t fan, char cmd, uint16_t minutes);

    const FanState& state(uint8_t fan);
    bool     isNight();             // now_night (по расписанию)
    bool     nightEffective();      // с учётом ручного переключателя
    uint16_t lastCo2();             // co2_send_data.CO2level
    uint32_t lastCo2UptimeS();      // CO2_last_time (аптайм, с; 0 — не было)
    uint16_t average();
    int8_t   speedPrevious();       // fan_speed_previous
    uint8_t  baseSpeed();           // общая скорость после поправки и ночного ограничения
    bool     radioOk();
    uint32_t secondsToNext(uint32_t now);
    // Режим связи переключается сам, когда меняются Cfg::co2.radioMode / passiveChannel / вентиляторы.
    // true один раз после каждой перенастройки радио (main: обновить отчёт о регистрах)
    bool     radioReconfigured();
    uint32_t passiveRxCount();      // принято запросов в пассивном режиме
}
