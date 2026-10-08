// radio.h — nRF24L01+ на библиотеке nrf24/RF24. Два режима связи с вентиляторами:
//   активный  — хаб передаёт каждому вентилятору (протокол хаба Old/ESP8266_WIFI/app/nrf24l01.c);
//   пассивный — хаб слушает, вентиляторы сами запрашивают, ответ — Payload with ACK, Dynamic Payload Length
//               (протокол хаба Old/CO2UART/app/nrf24l01.c, wireless_co2.c).
// CE модуля — перемычкой на 3.3 В (вывод CE не управляется).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

// Пакет вентилятору — CO2_SEND_DATA старых хабов, побайтно (ESP8266 и ESP32 little-endian).
// Активный режим: Param = Flags (хаб ESP8266 пересылал флаги датчика AVR; теперь всегда 0).
// Пассивный режим: Param = Pause — пауза вентилятора до следующего запроса, с.
struct __attribute__((packed)) SendData {
    uint16_t CO2level;   // ppm, текущее значение
    uint8_t  FanSpeed;   // 0..FAN_SPEED_MAX
    uint8_t  Param;      // Flags / Pause
};
static_assert(sizeof(SendData) == RADIO_PAYLOAD_LEN, "Размер пакета должен быть 4 байта");

enum class TxResult : uint8_t {
    Busy,        // передача идёт
    Ok,          // получен ACK
    NoAck,       // MAX_RT — вентилятор не ответил (NRF24_Transmit_Error хаба)
    NoResponse,  // модуль не ответил за RADIO_TX_TIMEOUT_MS (NRF24_Transmit_Timeout хаба) / не активный режим
    AddrFail,    // адрес не записался (NRF-SetAddr error хаба)
};

namespace Radio {
    bool     begin(uint8_t channel);          // активный режим; false — модуль не найден
    bool     isChipOk();
    bool     isPassive();
    // Сверка регистров с эталоном текущего режима; отчёт пишется в buf
    bool     verifyRegisters(char* buf, size_t len);

    // --- активный режим ---
    void     setActive();
    // Начать передачу: адрес {addrLsb, C8, C8}, канал вентилятора (переключается при смене)
    TxResult startTx(uint8_t addrLsb, uint8_t channel, const SendData& pkt, uint32_t now);
    TxResult poll(uint32_t now);              // вызывать до результата != Busy

    // --- пассивный режим ---
    // Приём на общем channel; канал приёма (pipe) i — адрес {addrLsb[i], C8, C8}, pipes <= RADIO_PIPES_MAX.
    // false — адрес не записался
    bool     setPassive(uint8_t channel, const uint8_t* addrLsb, uint8_t pipes);
    void     flushAcks();                                // убрать все подготовленные ответы
    bool     loadAck(uint8_t pipe, const SendData& pkt); // ответ на следующий запрос pipe; false — TX FIFO полон
    // Принятый запрос (неблокирующий): pipe, байты (до maxLen), len; false — ничего нет
    bool     receive(uint8_t& pipe, uint8_t* buf, uint8_t maxLen, uint8_t& len);
}
