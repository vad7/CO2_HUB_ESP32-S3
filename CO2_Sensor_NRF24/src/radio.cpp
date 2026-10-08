// radio.cpp — nRF24L01+ через nrf24/RF24 1.6.2, два режима связи с вентиляторами.
//
// Активный (хаб ESP8266, Old/ESP8266_WIFI/app/nrf24l01.c, NRF24_init + SetMode(TX)):
//   SETUP_AW=0x01 (3 байта)  SETUP_RETR=0x2F  RF_SETUP=0x07 (1 Мбит/с, 0 dBm)
//   EN_AA=0x01  EN_RXADDR=0x01  RX_PW_P0=4  CONFIG=0x7E (CRC16, IRQ замаскированы, PWR_UP, PTX)  DYNPD=0  FEATURE=0
//   Передача неблокирующая: RF24::write() ждёт до 95 мс, поэтому startFastWrite() + опрос STATUS.
// Пассивный (хаб CO2UART, Old/CO2UART/app/nrf24l01.c): PRX на общем канале, FEATURE = EN_DPL | EN_ACK_PAY,
//   канал приёма (pipe) i — вентилятор i; ответ {CO2, скорость, пауза} заранее кладётся в TX FIFO
//   (W_ACK_PAYLOAD) и уходит вместе с ACK на запрос вентилятора. CONFIG=0x7F (PRIM_RX).
//
// CE модуля — перемычкой на 3.3 В на обеих платах (так же работал хаб CO2UART: CE поднимался при старте).
// RF24 в «3-проводном» режиме (CE == CSN) — ce() ничего не делает. Следствия:
//   * активный: при пустом TX FIFO модуль в Standby-II (запись регистров разрешена), загрузка пакета сразу
//     запускает передачу; после MAX_RT пакет остаётся в FIFO и при снятии флага ушёл бы снова — поэтому
//     в finishTx() сначала flush_tx(), потом clearStatusFlags();
//   * пассивный: при PRIM_RX = 1 модуль сразу в приёме;
//   * смена режима — через выключение питания модуля (powerDown -> настройка -> powerUp): при CE = 1 выйти из
//     приёма в передачу можно только так (так же RF24 делает в 3-проводном режиме ATtiny). powerUp() внутри
//     RF24 ждёт 5 мс — только при смене режима и перезапуске модуля (действия пользователя / раз в час).
#include "radio.h"
#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <stdio.h>

// Доступ к защищённому read_register() — для сверки регистров и проверки адреса
class RF24Ext : public RF24 {
public:
    using RF24::RF24;
    uint8_t readReg(uint8_t reg)                          { return read_register(reg); }
    void    readRegs(uint8_t reg, uint8_t* buf, uint8_t n) { read_register(reg, buf, n); }
};

static SPIClass s_spi(NRF_SPI_BUS);
static RF24Ext  s_radio(NRF_PIN_CSN, NRF_PIN_CSN, RADIO_SPI_HZ);   // CE == CSN: RF24 не управляет CE
static bool     s_chipOk         = false;
static bool     s_passive        = false;
static bool     s_busy           = false;
static uint8_t  s_channel        = 0;       // канал активного режима (последний вентилятор)
static uint8_t  s_passiveChannel = 0;
static uint8_t  s_pipes          = 0;       // открытых каналов приёма (пассивный)
static uint32_t s_txStart        = 0;

// Эталонные значения регистров
constexpr uint8_t REG_CONFIG_TX    = 0x7E;
constexpr uint8_t REG_CONFIG_RX    = 0x7F;   // + PRIM_RX
constexpr uint8_t REG_EN_AA        = 0x01;
constexpr uint8_t REG_EN_AA_ALL    = 0x3F;
constexpr uint8_t REG_EN_RXADDR    = 0x01;
constexpr uint8_t REG_SETUP_AW     = RADIO_ADDR_WIDTH - 2;
constexpr uint8_t REG_SETUP_RETR   = (RADIO_RETRY_DELAY << 4) | RADIO_RETRY_COUNT;
constexpr uint8_t REG_RF_SETUP     = 0x07;
constexpr uint8_t RF_SETUP_MASK    = 0x2E;   // RF_DR_LOW, RF_DR_HIGH, RF_PWR (бит 0 у nRF24L01+ не используется)
constexpr uint8_t REG_RX_PW_P0     = RADIO_PAYLOAD_LEN;
constexpr uint8_t REG_DYNPD        = 0x00;
constexpr uint8_t REG_DYNPD_ALL    = 0x3F;
constexpr uint8_t REG_FEATURE      = 0x00;
constexpr uint8_t REG_FEATURE_ACK  = 0x06;   // EN_DPL | EN_ACK_PAY
constexpr uint8_t PIPE0            = 0;
constexpr uint8_t PIPE1            = 1;

namespace Radio {

static void makeAddr(uint8_t lsb, uint8_t* addr)
{
    addr[0] = lsb;
    addr[1] = RADIO_ADDR_BASE1;
    addr[2] = RADIO_ADDR_BASE2;
}

// Настройки, общие для обоих режимов (модуль в Power Down)
static void commonSetup()
{
    s_radio.setAddressWidth(RADIO_ADDR_WIDTH);
    s_radio.setRetries(RADIO_RETRY_DELAY, RADIO_RETRY_COUNT);
    s_radio.setDataRate(RF24_1MBPS);
    s_radio.setPALevel(RF24_PA_MAX, true);
    s_radio.setCRCLength(RF24_CRC_16);
    s_radio.maskIRQ(true, true, true);     // IRQ не используется
}

void setActive()
{
    if (!s_chipOk) return;
    s_radio.powerDown();
    commonSetup();
    s_radio.disableAckPayload();
    s_radio.disableDynamicPayloads();      // FEATURE = 0, DYNPD = 0
    s_radio.setPayloadSize(RADIO_PAYLOAD_LEN);
    s_radio.setAutoAck(false);
    s_radio.setAutoAck(PIPE0, true);       // ACK только на pipe 0
    for (uint8_t p = PIPE1; p < RADIO_PIPES_MAX; p++) s_radio.closeReadingPipe(p);
    s_radio.setChannel(s_channel);
    s_radio.flush_rx();
    s_radio.flush_tx();
    s_radio.powerUp();
    s_radio.stopListening();               // PTX; включает pipe 0 (EN_RXADDR = 0x01)
    s_radio.clearStatusFlags();
    s_passive = false;
    s_busy    = false;
}

bool begin(uint8_t channel)
{
    s_spi.begin(NRF_PIN_SCK, NRF_PIN_MISO, NRF_PIN_MOSI);   // CSN управляет RF24 программно
    pinMode(NRF_PIN_CSN, OUTPUT);       // в режиме CE == CSN RF24::begin() не настраивает выводы
    digitalWrite(NRF_PIN_CSN, HIGH);
    s_chipOk = s_radio.begin(&s_spi);   // внутри: задержка 5 мс, сброс FIFO, PWR_UP
    if (!s_chipOk) return false;
    s_channel = channel;
    setActive();
    return true;
}

bool isChipOk()  { return s_chipOk; }
bool isPassive() { return s_passive; }

bool setPassive(uint8_t channel, const uint8_t* addrLsb, uint8_t pipes)
{
    if (!s_chipOk) return false;
    if (pipes > RADIO_PIPES_MAX) pipes = RADIO_PIPES_MAX;
    s_radio.powerDown();
    commonSetup();
    s_radio.setAutoAck(true);              // EN_AA = 0x3F
    s_radio.enableDynamicPayloads();       // FEATURE.EN_DPL, DYNPD = 0x3F
    s_radio.enableAckPayload();            // FEATURE.EN_ACK_PAY
    uint8_t addr[RADIO_ADDR_WIDTH];
    for (uint8_t p = 0; p < RADIO_PIPES_MAX; p++) {
        if (p < pipes) {
            makeAddr(addrLsb[p], addr);    // pipe 2..5: пишется только младший байт, старшие — общие с pipe 1
            s_radio.openReadingPipe(p, addr);
        } else {
            s_radio.closeReadingPipe(p);
        }
    }
    s_radio.setChannel(channel);
    s_radio.flush_rx();
    s_radio.flush_tx();
    s_radio.startListening();              // powerUp + PRIM_RX (+ адрес pipe 0); при CE = 1 — сразу приём
    s_passiveChannel = channel;
    s_pipes   = pipes;
    s_passive = true;
    s_busy    = false;
    if (pipes == 0) return true;
    uint8_t check[RADIO_ADDR_WIDTH] = {0};
    s_radio.readRegs(nRF24L01::RX_ADDR_P0, check, RADIO_ADDR_WIDTH);
    makeAddr(addrLsb[0], addr);
    return memcmp(addr, check, RADIO_ADDR_WIDTH) == 0;
}

void flushAcks()
{
    if (s_chipOk && s_passive) s_radio.flush_tx();
}

bool loadAck(uint8_t pipe, const SendData& pkt)
{
    if (!s_chipOk || !s_passive || pipe >= s_pipes) return false;
    return s_radio.writeAckPayload(pipe, &pkt, sizeof(pkt));
}

bool receive(uint8_t& pipe, uint8_t* buf, uint8_t maxLen, uint8_t& len)
{
    if (!s_chipOk || !s_passive) return false;
    uint8_t p;
    if (!s_radio.available(&p)) return false;
    uint8_t n = s_radio.getDynamicPayloadSize();   // > 32 — RF24 сам очищает RX FIFO и возвращает 0
    if (n == 0) return false;
    uint8_t tmp[RADIO_MAX_PAYLOAD];
    s_radio.read(tmp, n);                          // снимает RX_DR
    s_radio.clearStatusFlags();                    // TX_DS после отправки ACK с данными, MAX_RT
    len  = n < maxLen ? n : maxLen;
    memcpy(buf, tmp, len);
    pipe = p;
    return true;
}

bool verifyRegisters(char* buf, size_t len)
{
    struct Check { const char* name; uint8_t reg; uint8_t expect; uint8_t mask; };
    const uint8_t rxPipes = (uint8_t)((1u << s_pipes) - 1);
    const Check active[] = {
        {"CONFIG",     nRF24L01::CONFIG,     REG_CONFIG_TX,  0xFF},
        {"EN_AA",      nRF24L01::EN_AA,      REG_EN_AA,      0x3F},
        {"EN_RXADDR",  nRF24L01::EN_RXADDR,  REG_EN_RXADDR,  0x3F},
        {"SETUP_AW",   nRF24L01::SETUP_AW,   REG_SETUP_AW,   0x03},
        {"SETUP_RETR", nRF24L01::SETUP_RETR, REG_SETUP_RETR, 0xFF},
        {"RF_CH",      nRF24L01::RF_CH,      s_channel,      0x7F},
        {"RF_SETUP",   nRF24L01::RF_SETUP,   REG_RF_SETUP,   RF_SETUP_MASK},
        {"RX_PW_P0",   nRF24L01::RX_PW_P0,   REG_RX_PW_P0,   0x3F},
        {"DYNPD",      nRF24L01::DYNPD,      REG_DYNPD,      0x3F},
        {"FEATURE",    nRF24L01::FEATURE,    REG_FEATURE,    0x07},
    };
    const Check passive[] = {
        {"CONFIG",     nRF24L01::CONFIG,     REG_CONFIG_RX,    0xFF},
        {"EN_AA",      nRF24L01::EN_AA,      REG_EN_AA_ALL,    0x3F},
        {"EN_RXADDR",  nRF24L01::EN_RXADDR,  rxPipes,          0x3F},
        {"SETUP_AW",   nRF24L01::SETUP_AW,   REG_SETUP_AW,     0x03},
        {"RF_CH",      nRF24L01::RF_CH,      s_passiveChannel, 0x7F},
        {"RF_SETUP",   nRF24L01::RF_SETUP,   REG_RF_SETUP,     RF_SETUP_MASK},
        {"DYNPD",      nRF24L01::DYNPD,      REG_DYNPD_ALL,    0x3F},
        {"FEATURE",    nRF24L01::FEATURE,    REG_FEATURE_ACK,  0x07},
    };
    const Check* checks = s_passive ? passive : active;
    const size_t n = s_passive ? sizeof(passive) / sizeof(passive[0]) : sizeof(active) / sizeof(active[0]);
    bool ok = s_chipOk;
    size_t pos = snprintf(buf, len, "nRF24 %s%s:", s_chipOk ? "OK" : "НЕ НАЙДЕН", s_passive ? " (пассивный)" : "");
    if (!s_chipOk) return false;
    for (size_t i = 0; i < n; i++) {
        const Check& c = checks[i];
        uint8_t v = s_radio.readReg(c.reg);
        bool match = (v & c.mask) == (c.expect & c.mask);
        ok &= match;
        if (pos < len) pos += snprintf(buf + pos, len - pos, " %s=%02X%s", c.name, v, match ? "" : "(!)");
    }
    return ok;
}

TxResult startTx(uint8_t addrLsb, uint8_t channel, const SendData& pkt, uint32_t now)
{
    if (!s_chipOk || s_passive) return TxResult::NoResponse;

    uint8_t addr[RADIO_ADDR_WIDTH];
    makeAddr(addrLsb, addr);
    s_radio.openWritingPipe(addr);         // TX_ADDR и RX_ADDR_P0, младший байт первым
    uint8_t check[RADIO_ADDR_WIDTH] = {0};
    s_radio.readRegs(nRF24L01::TX_ADDR, check, RADIO_ADDR_WIDTH);
    if (memcmp(addr, check, RADIO_ADDR_WIDTH) != 0) return TxResult::AddrFail;

    if (channel != s_channel) {               // set_new_rf_channel() хаба: писать только при смене
        s_radio.setChannel(channel);
        s_channel = channel;
    }

    s_radio.clearStatusFlags();
    s_radio.flush_tx();
    s_radio.startFastWrite(&pkt, sizeof(pkt), false, true);   // CE = 1 аппаратно — передача сразу
    s_txStart = now;
    s_busy = true;
    return TxResult::Busy;
}

static TxResult finishTx(TxResult res)
{
    if (res != TxResult::Ok) s_radio.flush_tx();   // до снятия MAX_RT: иначе пакет ушёл бы повторно
    s_radio.clearStatusFlags();
    s_busy = false;
    return res;
}

TxResult poll(uint32_t now)
{
    if (!s_busy) return TxResult::NoResponse;
    uint8_t st = s_radio.update();
    if (st & RF24_TX_DS) return finishTx(TxResult::Ok);
    if (st & RF24_TX_DF) return finishTx(TxResult::NoAck);
    if (now - s_txStart > RADIO_TX_TIMEOUT_MS) return finishTx(TxResult::NoResponse);
    return TxResult::Busy;
}

} // namespace Radio
