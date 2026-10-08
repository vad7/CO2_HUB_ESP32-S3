// fan_control.cpp — перенос CO2_Averaging(), CO2_set_fans_speed_current(), user_loop(),
// send_fans_speed_now() хаба (Old/ESP8266_WIFI/app/wireless_co2.c).
// Отличие: CO2 берётся с собственного датчика (K22 или S8) раз в transmitPeriodS (у хаба — по приходу пакета датчика).
//
// Режимы связи (Cfg::co2.radioMode, переключаются на лету — по изменению настроек):
//   активный  — раз в transmitPeriodS (и по sendNow) пакет каждому вентилятору на его канале (хаб ESP8266);
//   пассивный — вентилятор i шлёт запрос (1 байт состояния) на канал приёма i общего канала passiveChannel,
//               хаб отвечает в ACK заранее подготовленным {CO2, скорость, пауза} (хаб Old/CO2UART, user_loop).
//               Вентиляторов в пассивном режиме — до PASSIVE_FANS_MAX (3) = TX FIFO nRF24: ответ лежит наготове
//               для каждого; после запроса ответ этому вентилятору кладётся снова, новые CO2/скорость
//               (раз в transmitPeriodS, sendNow) — сброс и загрузка заново. Вентиляторы с номера 4 — без связи.
#include "fan_control.h"
#include <Arduino.h>
#include <time.h>
#include "hub_config.h"
#include "radio.h"
#include "history.h"
#include "temp_sensor.h"
#include "net.h"

namespace FanControl {

constexpr uint8_t  AVG_NOT_INIT  = CO2_AVERAGE_LEN;   // CO2LevelAverageIdx до первого замера
constexpr uint16_t HHMM_DIV      = 100;
constexpr uint16_t MIN_PER_HOUR  = 60;
constexpr int      WDAY_SUNDAY   = 0;
constexpr int      WDAY_SATURDAY = 6;
constexpr uint32_t MS_PER_S      = 1000;
constexpr uint32_t S_PER_MIN     = 60;

static FanState  s_state[FANS_MAX];
static uint16_t  s_avgArray[CO2_AVERAGE_LEN];
static uint8_t   s_avgIdx       = AVG_NOT_INIT;
static uint16_t  s_average      = 0;
static int8_t    s_speedPrev    = 0;     // fan_speed_previous
static uint8_t   s_baseSpeed    = 0;
static bool      s_night        = false; // now_night
static uint16_t  s_co2          = 0;
static uint32_t  s_co2UptimeS   = 0;
static uint32_t  s_nextCycle    = 0;
static uint32_t  s_nextTick     = 0;
static uint16_t  s_pending      = 0;     // битовая маска вентиляторов к отправке
static bool      s_sending      = false;
static uint8_t   s_sendFan      = 0;
static_assert(FANS_MAX <= 16, "s_pending — 16 бит");

// Пассивный режим
constexpr uint32_t FNV_OFFSET   = 2166136261u;   // FNV-1a: подпись настроек радио
constexpr uint32_t FNV_PRIME    = 16777619u;
constexpr uint8_t  RX_PER_CALL  = 4;             // запросов за один update()
static uint32_t  s_radioSig     = 0;             // подпись применённых настроек радио
static bool      s_radioSigSet  = false;
static bool      s_reconfigured = false;
static uint8_t   s_pipes        = 0;             // каналов приёма (вентиляторов) в пассивном режиме
static uint8_t   s_ackLoaded    = 0;             // маска каналов, для которых ответ лежит в TX FIFO
static uint8_t   s_ackNext      = 0;             // с какого канала начинать загрузку (по кругу)
static bool      s_ackReload    = false;         // сбросить ответы и загрузить заново (новые данные)
static uint32_t  s_lastRxS      = 0;             // аптайм последнего запроса / перенастройки, с
static uint32_t  s_rxCount      = 0;
static_assert(RADIO_PIPES_MAX <= 8, "s_ackLoaded — 8 бит");

void begin(uint32_t now)
{
    memset(s_state, 0, sizeof(s_state));
    s_nextCycle = now + FIRST_TRANSMIT_MS;
    s_nextTick  = now + FAN_TICK_MS;
}

static uint32_t fnv(uint32_t h, uint8_t b) { return (h ^ b) * FNV_PRIME; }

// Подпись настроек, от которых зависит конфигурация модуля
static uint32_t radioSignature()
{
    uint32_t h = fnv(FNV_OFFSET, Cfg::co2.radioMode);
    if (Cfg::co2.radioMode == RADIO_PASSIVE) {
        h = fnv(h, Cfg::co2.passiveChannel);
        h = fnv(h, Cfg::co2.fans);
        for (uint8_t i = 0; i < Cfg::co2.fans && i < PASSIVE_FANS_MAX; i++) h = fnv(h, Cfg::fans[i].addressLsb);
    }
    return h;
}

// Перенастройка модуля под текущий режим; состояния вентиляторов (кроме скорости) сбрасываются
static void applyRadio()
{
    for (FanState& st : s_state) {
        st.txStatus = TX_NONE;
        st.txOkUptimeS = 0;
        st.remoteStatus = 0;
        st.remoteAdjust = 0;
        st.remoteOff = st.remoteEeprom = false;
    }
    s_sending = false;
    s_ackLoaded = 0;
    s_lastRxS = Net::uptimeS();
    if (Cfg::co2.radioMode == RADIO_PASSIVE) {
        uint8_t addr[PASSIVE_FANS_MAX];
        s_pipes = Cfg::co2.fans < PASSIVE_FANS_MAX ? Cfg::co2.fans : PASSIVE_FANS_MAX;   // остальные — без связи
        for (uint8_t i = 0; i < s_pipes; i++) addr[i] = Cfg::fans[i].addressLsb;
        if (!Radio::setPassive(Cfg::co2.passiveChannel, addr, s_pipes)) Serial.println("nRF24: адрес приёма не записался");
        s_ackReload = true;
        Serial.printf("nRF24: пассивный режим, канал %u, вентиляторов %u\n", Cfg::co2.passiveChannel, s_pipes);
    } else {
        s_pipes = 0;
        Radio::setActive();
        sendNow(FAN_ALL, false);
        Serial.println("nRF24: активный режим");
    }
    s_reconfigured = true;
}

// CO2_Averaging()
static void averaging(uint16_t co2)
{
    if (s_avgIdx == AVG_NOT_INIT) {
        for (uint8_t i = 1; i < CO2_AVERAGE_LEN; i++) s_avgArray[i] = co2;
    }
    if (++s_avgIdx >= CO2_AVERAGE_LEN) s_avgIdx = 0;
    s_avgArray[s_avgIdx] = co2;
}

static uint16_t hhmmToMin(uint16_t v) { return (v / HHMM_DIV) * MIN_PER_HOUR + v % HHMM_DIV; }

// now_night по расписанию; без синхронизированного времени — «не ночь»
static bool calcNight()
{
    if (!Net::timeValid()) return false;
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    uint16_t st, end, tt = tm.tm_hour * MIN_PER_HOUR + tm.tm_min;
    if (tm.tm_wday == WDAY_SUNDAY || tm.tm_wday == WDAY_SATURDAY) {
        st  = Cfg::co2.nightStartWd;
        end = Cfg::co2.nightEndWd;
    } else {
        st  = Cfg::co2.nightStart;
        end = Cfg::co2.nightEnd;
    }
    st  = hhmmToMin(st);
    end = hhmmToMin(end);
    return (end > st && tt >= st && tt <= end) || (end < st && (tt >= st || tt <= end));
}

bool nightEffective()
{
    return Cfg::nightOverride == NIGHT_FORCE_ON  ? true
         : Cfg::nightOverride == NIGHT_FORCE_OFF ? false
         : s_night;
}

static int8_t applyOverride(int8_t fsp, uint8_t mode, int8_t speed)
{
    switch (mode) {
    case OVR_SET: return speed;
    case OVR_ADD: return (int8_t)(fsp + speed);
    case OVR_MAX: return fsp > speed ? speed : fsp;
    case OVR_MIN: return fsp < speed ? speed : fsp;
    default:      return fsp;
    }
}

// CO2_set_fans_speed_current(); nfan = FAN_ALL — все
static void calcSpeeds(uint8_t nfan)
{
    uint32_t sum = 0;
    for (uint8_t i = 0; i < CO2_AVERAGE_LEN; i++) sum += s_avgArray[i];
    s_average = (uint16_t)(sum / CO2_AVERAGE_LEN);

    int8_t speed;
    for (speed = 0; speed < FAN_SPEED_MAX; speed++) {
        uint16_t tr = Cfg::co2.thresholds[speed];
        if (s_average < tr) {
            // при снижении CO2 — понижать скорость только с запасом fans_speed_delta
            if (s_speedPrev <= speed || (uint16_t)(tr - s_average) >= Cfg::co2.speedDelta) break;
        }
    }
    s_speedPrev = speed;
    speed += Cfg::vars.speedOverride;
    if (speed > FAN_SPEED_MAX) speed = FAN_SPEED_MAX;
    if (speed < 0) speed = 0;

    s_night = calcNight();
    const bool night = nightEffective();
    if (night && speed > (int8_t)Cfg::co2.nightMaxSpeed) speed = (int8_t)Cfg::co2.nightMaxSpeed;
    s_baseSpeed = (uint8_t)speed;

    uint8_t from = 0, to = Cfg::co2.fans;
    if (nfan != FAN_ALL) { from = nfan; to = nfan + 1; }
    for (uint8_t fan = from; fan < to && fan < FANS_MAX; fan++) {
        const CfgFan& f = Cfg::fans[fan];
        if (f.flags & FAN_FLAG_FORCED) continue;
        int8_t fsp = night ? applyOverride(speed, f.overrideNight, f.speedNight)
                           : applyOverride(speed, f.overrideDay,   f.speedDay);
        if (fsp < f.speedMin) fsp = f.speedMin;
        if (fsp > f.speedMax) fsp = f.speedMax;
        if (fsp < 0) fsp = 0;
        s_state[fan].speedCurrent = (uint8_t)fsp;
    }
}

void sendNow(uint8_t fan, bool calcSpeed)
{
    if (calcSpeed) calcSpeeds(fan);
    if (Cfg::co2.radioMode == RADIO_PASSIVE) { s_ackReload = true; return; }   // ответы в ACK — заново
    if (fan == FAN_ALL) s_pending = (uint16_t)((1u << FANS_MAX) - 1);
    else if (fan < FANS_MAX) s_pending |= (uint16_t)(1u << fan);
}

void fanOverride(uint8_t fan, char cmd, uint16_t minutes)
{
    if (fan >= FANS_MAX) return;
    CfgFan&   f  = Cfg::fans[fan];
    FanState& st = s_state[fan];
    int16_t sp = st.speedCurrent;
    if (cmd == 'p')      { sp++; f.flags |= FAN_FLAG_FORCED; }
    else if (cmd == 'm') { sp--; f.flags |= FAN_FLAG_FORCED; }
    else                 { f.flags &= (uint8_t)~FAN_FLAG_FORCED; }
    if (sp < f.speedMin) sp = f.speedMin;
    if (sp > f.speedMax) sp = f.speedMax;
    st.speedCurrent   = (uint8_t)sp;
    st.forcedTimeoutS = (uint32_t)minutes * S_PER_MIN;
    sendNow(fan, !(f.flags & FAN_FLAG_FORCED));
}

// Таймауты принудительной скорости и (пассивный) молчания вентиляторов — раз в секунду (user_loop хаба)
static void tick()
{
    const uint32_t up = Net::uptimeS();
    for (uint8_t fan = 0; fan < Cfg::co2.fans; fan++) {
        CfgFan& f = Cfg::fans[fan];
        FanState& st = s_state[fan];
        if ((f.flags & FAN_FLAG_FORCED) && st.forcedTimeoutS) {
            if (--st.forcedTimeoutS == 0) f.flags &= (uint8_t)~FAN_FLAG_FORCED;
        }
        if (Cfg::co2.radioMode == RADIO_PASSIVE && f.timeoutS && st.txOkUptimeS && !st.remoteOff &&
            up - st.txOkUptimeS > f.timeoutS) st.txStatus = TX_TIMEOUT;   // «+8 — silence» хаба CO2UART
    }
}

// --- пассивный режим ---
static SendData ackPacket(uint8_t fan)
{
    SendData pkt;
    pkt.CO2level = s_co2;
    pkt.FanSpeed = s_state[fan].speedCurrent;
    pkt.Param    = Cfg::fans[fan].pauseS;
    return pkt;
}

// Загрузить недостающие ответы в TX FIFO по кругу, пока есть место
static void refillAcks()
{
    if (s_co2UptimeS == 0) return;   // CO2 ещё не измерен — без ответа (хаб CO2UART: if(CO2level))
    for (uint8_t i = 0; i < s_pipes; i++) {
        const uint8_t fan = (uint8_t)((s_ackNext + i) % s_pipes);
        const uint8_t bit = (uint8_t)(1u << fan);
        if ((s_ackLoaded & bit) || (Cfg::fans[fan].flags & FAN_FLAG_SKIP)) continue;
        if (!Radio::loadAck(fan, ackPacket(fan))) break;   // FIFO полон — остальные после следующих запросов
        s_ackLoaded |= bit;
    }
    if (s_pipes) s_ackNext = (uint8_t)((s_ackNext + 1) % s_pipes);
}

// Запрос от вентилятора: байт состояния (Old/CO2UART: user_loop)
static void onRequest(uint8_t fan, uint8_t st)
{
    FanState& s = s_state[fan];
    s.txOkUptimeS  = Net::uptimeS();
    s.remoteStatus = st;
    if (st == FAN_ST_EEPROM_BROKEN) {
        s.remoteEeprom = true;
        s.txStatus = TX_ERROR;
        return;
    }
    s.remoteEeprom = false;
    s.remoteOff    = (st & FAN_ST_OFF_BIT) != 0;
    int8_t adj = (int8_t)(st & FAN_ST_ADJ_MASK);
    if (adj & FAN_ST_ADJ_SIGN) adj = (int8_t)(adj - 2 * FAN_ST_ADJ_SIGN);   // 4-битное со знаком
    s.remoteAdjust = adj;
    s.txStatus = ((st >> FAN_ST_TX_SHIFT) & FAN_ST_TX_MASK) == 0 ? TX_OK : TX_ERROR;
}

static void passiveStep()
{
    bool refill = false;
    if (s_ackReload) {
        s_ackReload = false;
        Radio::flushAcks();
        s_ackLoaded = 0;
        refill = true;
    }
    uint8_t pipe, len, buf[RADIO_MAX_PAYLOAD];
    for (uint8_t n = 0; n < RX_PER_CALL && Radio::receive(pipe, buf, sizeof(buf), len); n++) {
        s_rxCount++;
        s_lastRxS = Net::uptimeS();
        if (pipe >= s_pipes) continue;
        onRequest(pipe, buf[0]);
        s_ackLoaded &= (uint8_t)~(1u << pipe);   // ответ ушёл с ACK этого запроса — положить следующий
        refill = true;
    }
    if (refill) refillAcks();
    // нет приёма дольше radioResetS — перезапуск модуля (nRF24_reset_time хаба CO2UART)
    if (Cfg::co2.radioResetS && Net::uptimeS() - s_lastRxS > Cfg::co2.radioResetS) {
        Serial.println("nRF24: нет запросов — перезапуск модуля");
        applyRadio();
    }
}

static void sendStep(uint32_t now)
{
    if (s_sending) {
        TxResult r = Radio::poll(now);
        if (r == TxResult::Busy) return;
        FanState& st = s_state[s_sendFan];
        st.txStatus = r == TxResult::Ok ? TX_OK : r == TxResult::NoAck ? TX_ERROR : TX_TIMEOUT;
        if (r == TxResult::Ok) st.txOkUptimeS = Net::uptimeS();
        s_sending = false;
        return;
    }
    while (s_pending) {
        uint8_t fan = (uint8_t)__builtin_ctz(s_pending);
        s_pending &= (uint16_t)~(1u << fan);
        if (fan >= Cfg::co2.fans) { s_pending = 0; break; }
        const CfgFan& f = Cfg::fans[fan];
        if (f.flags & FAN_FLAG_SKIP) continue;
        SendData pkt;
        pkt.CO2level = s_co2;
        pkt.FanSpeed = s_state[fan].speedCurrent;
        pkt.Param    = 0;                    // Flags хаба ESP8266
        TxResult r = Radio::startTx(f.addressLsb, f.rfChannel, pkt, now);
        if (r == TxResult::Busy) {
            s_sendFan = fan;
            s_sending = true;
            return;
        }
        s_state[fan].txStatus = TX_TIMEOUT;   // модуля нет / адрес не записался
    }
}

void update(uint32_t now, uint16_t co2, bool co2Valid)
{
    const uint32_t sig = radioSignature();   // режим / канал / вентиляторы изменены из меню или веба
    if (!s_radioSigSet || sig != s_radioSig) {
        s_radioSig = sig;
        s_radioSigSet = true;
        if (Radio::isChipOk()) applyRadio();
    }
    if ((int32_t)(now - s_nextTick) >= 0) {
        s_nextTick += FAN_TICK_MS;
        tick();
    }
    if ((int32_t)(now - s_nextCycle) >= 0 && co2Valid) {
        s_nextCycle = now + (uint32_t)Cfg::co2.transmitPeriodS * MS_PER_S;
        s_co2 = co2;
        s_co2UptimeS = Net::uptimeS();
        averaging(co2);
        History::add(co2, TempSensor::valid() ? TempSensor::tenthsC() : HISTORY_NO_TEMP, s_co2UptimeS);   // температура — в тот же момент
        sendNow(FAN_ALL, true);
    }
    if (Radio::isPassive()) passiveStep();
    else                    sendStep(now);
}

bool radioReconfigured()
{
    const bool r = s_reconfigured;
    s_reconfigured = false;
    return r;
}

uint32_t passiveRxCount() { return s_rxCount; }

const FanState& state(uint8_t fan)  { return s_state[fan < FANS_MAX ? fan : 0]; }
bool     isNight()                  { return s_night; }
uint16_t lastCo2()                  { return s_co2; }
uint32_t lastCo2UptimeS()           { return s_co2UptimeS; }
uint16_t average()                  { return s_average; }
int8_t   speedPrevious()            { return s_speedPrev; }
uint8_t  baseSpeed()                { return s_baseSpeed; }
bool     radioOk()                  { return Radio::isChipOk(); }

uint32_t secondsToNext(uint32_t now)
{
    int32_t d = (int32_t)(s_nextCycle - now);
    return d > 0 ? (uint32_t)d / MS_PER_S : 0;
}

} // namespace FanControl
