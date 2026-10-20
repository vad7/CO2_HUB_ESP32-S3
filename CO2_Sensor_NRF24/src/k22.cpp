// k22.cpp - датчик CO2 SenseAir K22-OC (CO2_SENSOR == CO2_SENSOR_K22) по I2C (Docs/SensAir_I2C_comm_guide_2_1031.pdf,
// текст - work/SensAir_I2C_comm_guide_2_1031.txt). Обмен - транзакция «запрос -> пауза tWAIT -> ответ» (гл. 4):
//   запрос: [команда<<4 | число байт] [адрес Hi] [адрес Lo] [данные - только запись] [сумма байт запроса]
//   ответ:  [команда<<4 | бит «выполнено»] [данные - только чтение] [сумма байт ответа]
// Неблокирующий автомат на millis(): CO2 - чтение RAM 0x08 раз в период опроса (настройка co2.co2PollS); между опросами - шаги ABC
// (чтение / запись EEPROM, гл. 8.4), запрошенные из веба.
// I2C через драйвер LovyanGFX: на ES3C28P шина I2C0 общая с тачем FT6336G,
// один драйвер на пинах исключает конфликт с Arduino Wire.
#include "config.h"
#if CO2_SENSOR == CO2_SENSOR_K22
#include "co2_sensor.h"
#include <stdio.h>
#include <string.h>
#include "lgfx_include.h"
#include "errlog.h"
#include "i2c_bus.h"

namespace Co2Sensor {

constexpr uint32_t MS_PER_S = 1000;

enum class State : uint8_t { Idle, WaitResponse };
// Шаг обмена: CO2 - опрос, Status - Error Status после него; остальные - ABC (Map - проверка карты памяти,
// Mc - байт MeterControl)
enum class Op : uint8_t { None, Co2, Status, Map, ReadMc, WriteMc, WritePeriod, ReadPeriod };

constexpr uint8_t TXN_DATA_MAX    = 2;                           // данных в одной транзакции (CO2, период ABC)
constexpr uint8_t REQ_HDR_LEN     = 3;                           // команда|число, адрес Hi, Lo
constexpr uint8_t REQ_MAX         = REQ_HDR_LEN + TXN_DATA_MAX + 1;
constexpr uint8_t RESP_MAX        = 1 + TXN_DATA_MAX + 1;        // статус, данные, сумма
constexpr uint8_t CMD_SHIFT       = 4;
constexpr uint8_t COUNT_MASK      = 0x0F;
constexpr uint8_t STATUS_COMPLETE = 0x01;                        // бит 0 статуса ответа - «выполнено»
constexpr uint8_t BITS_PER_BYTE   = 8;

struct Txn { uint8_t cmd; uint16_t addr; uint8_t n; uint8_t data[TXN_DATA_MAX]; };

static State    s_state       = State::Idle;        // ожидание ответа или простой
static Op       s_op          = Op::None;     // шаг, ожидающий ответа
static Txn      s_txn         = {};                 // текущая транзакция (запрос)
static uint8_t  s_resp[RESP_MAX];                   // ответ датчика: статус, данные, сумма
static uint32_t s_stateTime   = 0;                  // момент запроса / последнего чтения (millis)
static uint32_t s_periodMs  = (uint32_t)CO2_POLL_DEF_S * MS_PER_S;   // период опроса, мс (из update)
static uint32_t s_lastPoll    = 0;                  // последний опрос CO2 (millis)
static uint32_t s_lastValid   = 0;                  // последнее верное значение CO2 (millis)
static bool     s_everValid   = false;              // было хоть одно верное значение
static uint16_t s_co2         = 0;                  // последнее значение CO2, ppm
static uint8_t  s_retries     = 0;                  // повторов чтения ответа «не готов»
static uint32_t s_errors      = 0;                  // ошибок обмена всего
static Co2Err   s_lastErr     = Co2Err::None;       // ошибка последнего опроса (None - удачно)
static uint8_t  s_errStatus   = 0;            // Error Status (RAM 0x1E), 0 - без ошибок
static bool     s_statusNext  = false;        // после удачного чтения CO2 - прочитать Error Status
static bool     s_wasValid    = false;        // для записи «нет данных» в журнал при переходе
static uint8_t  s_failRun     = 0;            // неудачных опросов подряд (CO2 или Error Status ≠ 0); 0 - удачный

// ABC
static AbcState s_abcState    = AbcState::None;
static Op       s_abcNext     = Op::None;     // следующий шаг ABC (None - задачи нет)
static int32_t  s_wantPeriod  = -1;           // записать период, ч (-1 - не менять)
static int8_t   s_wantOn      = -1;           // записать вкл/выкл (-1 - не менять)
static uint8_t  s_mc          = 0;            // MeterControl из EEPROM
static uint8_t  s_mcNew       = 0;            // MeterControl к записи
static uint16_t s_period      = 0;            // ABC Period из EEPROM, ч
static bool     s_mcWritten   = false;        // MeterControl менялся - датчику нужен перезапуск питания
static char     s_abcMsg[ABC_MSG_LEN] = "";         // ABC: ошибка или примечание

void begin()
{
    lgfx::i2c::init(I2C_PORT, I2C_PIN_SDA, I2C_PIN_SCL);
    s_lastPoll = millis() - s_periodMs;   // первый опрос сразу
}

static bool isWrite(uint8_t cmd) { return cmd == K22_CMD_WRITE_RAM || cmd == K22_CMD_WRITE_EE; }
static uint8_t respLen(const Txn& t) { return (uint8_t)(1 + (isWrite(t.cmd) ? 0 : t.n) + 1); }

static bool sendTxn(const Txn& t)
{
    uint8_t req[REQ_MAX];
    uint8_t k = 0;
    req[k++] = (uint8_t)((t.cmd << CMD_SHIFT) | (t.n & COUNT_MASK));
    req[k++] = (uint8_t)(t.addr >> BITS_PER_BYTE);
    req[k++] = (uint8_t)t.addr;
    if (isWrite(t.cmd)) for (uint8_t i = 0; i < t.n; i++) req[k++] = t.data[i];
    uint8_t sum = 0;                              // сумма байт без адреса I2C (прил. A)
    for (uint8_t i = 0; i < k; i++) sum = (uint8_t)(sum + req[i]);
    req[k++] = sum;
    return lgfx::i2c::transactionWrite(I2C_PORT, K22_I2C_ADDR, req, k, I2cBus::hz()).has_value();
}

// 1 - выполнено (данные чтения - s_resp[1..n]), 0 - не готов (повторить), -1 - неверный ответ
static int8_t readTxn(const Txn& t)
{
    const uint8_t len = respLen(t);
    memset(s_resp, 0, sizeof(s_resp));
    if (!lgfx::i2c::transactionRead(I2C_PORT, K22_I2C_ADDR, s_resp, len, I2cBus::hz()).has_value()) return 0; // NACK = занят
    if (!(s_resp[0] & STATUS_COMPLETE)) return 0;
    if ((s_resp[0] >> CMD_SHIFT) != t.cmd) return -1;
    uint8_t sum = 0;
    for (uint8_t i = 0; i + 1 < len; i++) sum = (uint8_t)(sum + s_resp[i]);
    return sum == s_resp[len - 1] ? 1 : -1;
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << BITS_PER_BYTE) | p[1]); }

// Транзакция шага
static Txn txnFor(Op op)
{
    switch (op) {
    case Op::Map:         return { K22_CMD_READ_RAM, K22_RAM_MEMMAP, 1, {} };
    case Op::ReadMc:      return { K22_CMD_READ_EE, K22_EE_METER_CONTROL, 1, {} };
    case Op::WriteMc:     return { K22_CMD_WRITE_EE, K22_EE_METER_CONTROL, 1, { s_mcNew } };
    case Op::WritePeriod: return { K22_CMD_WRITE_EE, K22_EE_ABC_PERIOD, 2,
                                   { (uint8_t)(s_wantPeriod >> BITS_PER_BYTE), (uint8_t)s_wantPeriod } };
    case Op::ReadPeriod:  return { K22_CMD_READ_EE, K22_EE_ABC_PERIOD, 2, {} };
    case Op::Status:      return { K22_CMD_READ_RAM, K22_RAM_ERR_STATUS, 1, {} };
    default:              return { K22_CMD_READ_RAM, K22_RAM_CO2, 2, {} };
    }
}

static const char* opName(Op op)
{
    switch (op) {
    case Op::Map:         return "карта памяти";
    case Op::ReadMc:      return "чтение MeterControl";
    case Op::WriteMc:     return "запись MeterControl";
    case Op::WritePeriod: return "запись периода";
    case Op::ReadPeriod:  return "чтение периода";
    default:              return "?";
    }
}

static void abcFail(Op op, const char* why)
{
    snprintf(s_abcMsg, sizeof(s_abcMsg), "%s: %s", opName(op), why);
    s_abcState = AbcState::Error;
    s_abcNext  = Op::None;
}

// Шаг ABC выполнен: разбор ответа и выбор следующего шага.
// Запись: Map -> ReadMc -> [WriteMc] -> [WritePeriod] -> ReadMc -> ReadPeriod; чтение: Map -> ReadMc -> ReadPeriod
static void abcStepDone(Op op)
{
    switch (op) {
    case Op::Map: {
        const uint8_t map = s_resp[1];
        if (map != K22_MEMMAP_A && map != K22_MEMMAP_B) {
            char why[ABC_MSG_LEN];
            snprintf(why, sizeof(why), "0x%02X - адреса ABC для неё неизвестны (гайд: 0x%02X, 0x%02X)", map, K22_MEMMAP_A, K22_MEMMAP_B);
            abcFail(op, why);
            return;
        }
        s_abcNext = Op::ReadMc;
        return;
    }
    case Op::ReadMc:
        s_mc = s_resp[1];
        if (s_wantOn >= 0) {
            s_mcNew = s_wantOn ? (uint8_t)(s_mc & ~K22_MC_ABC_DISABLE) : (uint8_t)(s_mc | K22_MC_ABC_DISABLE);
            if (s_mcNew != s_mc) { s_abcNext = Op::WriteMc; return; }
            s_wantOn = -1;                         // уже как надо
        }
        s_abcNext = s_wantPeriod >= 0 ? Op::WritePeriod : Op::ReadPeriod;
        return;
    case Op::WriteMc:
        s_wantOn    = -1;
        s_mcWritten = true;
        s_abcNext   = s_wantPeriod >= 0 ? Op::WritePeriod : Op::ReadMc;
        return;
    case Op::WritePeriod:
        s_wantPeriod = -1;
        s_abcNext    = Op::ReadMc;                 // проверка: перечитать оба значения
        return;
    case Op::ReadPeriod:
        s_period    = be16(&s_resp[1]);
        s_abcNext   = Op::None;
        s_abcState  = AbcState::Ok;
        if (s_mcWritten) snprintf(s_abcMsg, sizeof(s_abcMsg), "вкл/выкл ABC вступит в силу после перезапуска питания датчика");
        else if (s_period == 0) snprintf(s_abcMsg, sizeof(s_abcMsg), "период 0 - ABC не работает");
        else s_abcMsg[0] = '\0';
        return;
    default:
        return;
    }
}

// Ошибка опроса (CO2 или Error Status): счётчик, последняя ошибка, журнал
static void co2Fail(Co2Err e)
{
    s_errors++;
    if (s_failRun < UINT8_MAX) s_failRun++;
    s_lastErr = e;
    ErrLog::add(ErrDev::Co2, (uint8_t)e);
}

static void startOp(Op op, uint32_t now)
{
    s_txn = txnFor(op);
    if (!sendTxn(s_txn)) {
        if (op == Op::Co2 || op == Op::Status) co2Fail(Co2Err::NoAnswer);
        else abcFail(op, "датчик не принял запрос (I2C)");
        return;
    }
    s_op        = op;
    s_retries   = 0;
    s_stateTime = now;
    s_state     = State::WaitResponse;
}

void update(uint32_t now, uint8_t periodS)
{
    s_periodMs = (uint32_t)periodS * MS_PER_S;   // настройка меняется на лету
    const bool valid = isValid(now);
    if (s_wasValid && !valid) ErrLog::add(ErrDev::Co2, (uint8_t)Co2Err::Stale);
    s_wasValid = valid;
    switch (s_state) {
    case State::Idle:
        if (s_statusNext) { s_statusNext = false; startOp(Op::Status, now); break; }
        if (s_abcNext != Op::None) { startOp(s_abcNext, now); break; }   // ABC - между опросами CO2
        if (now - s_lastPoll < s_periodMs) break;
        s_lastPoll = now;
        startOp(Op::Co2, now);
        break;

    case State::WaitResponse: {
        if (now - s_stateTime < K22_WAIT_MS) break;
        const int8_t r = readTxn(s_txn);
        if (r == 0 && ++s_retries < K22_READ_RETRIES) { s_stateTime = now; break; }
        s_state = State::Idle;
        if (s_op == Op::Co2) {
            if (r == 1) {
                const int16_t v = (int16_t)be16(&s_resp[1]);
                s_co2 = v < 0 ? 0 : (uint16_t)v;   // отрицательные бывают при тесте азотом
                s_lastValid = now;
                s_everValid = true;
                s_statusNext = true;
            } else {
                co2Fail(r == 0 ? Co2Err::NoAnswer : Co2Err::BadFrame);
            }
        } else if (s_op == Op::Status) {
            if (r == 1) {
                s_errStatus = s_resp[1];
                s_lastErr = s_errStatus ? Co2Err::Sensor : Co2Err::None;
                if (s_errStatus) {
                    ErrLog::add(ErrDev::Co2, (uint8_t)Co2Err::Sensor, s_errStatus);
                    if (s_failRun < UINT8_MAX) s_failRun++;
                } else {
                    s_failRun = 0;                     // опрос удачен: CO2 и статус без ошибок
                }
            } else {
                co2Fail(r == 0 ? Co2Err::NoAnswer : Co2Err::BadFrame);
            }
        } else if (r == 1) {
            abcStepDone(s_op);
        } else {
            abcFail(s_op, r == 0 ? "нет ответа (датчик без EEPROM не отвечает на её команды)" : "неверный ответ");
        }
        s_op = Op::None;
        break;
    }
    }
}

bool     isValid(uint32_t now) { return s_everValid && (now - s_lastValid) < CO2_FAIL_PERIODS * s_periodMs; }
uint16_t co2()                 { return s_co2; }
uint32_t errorCount()          { return s_errors; }

Co2Err   lastError()           { return s_lastErr; }
uint16_t sensorStatus()        { return s_errStatus; }
uint8_t  failRun()             { return s_failRun; }

// Error Status (RAM 0x1E): биты «зависят от продукта» (гайд, табл. 11), описания для K22 нет - только код
void statusBitsText(uint16_t status, char* buf, size_t len)
{
    if (status) snprintf(buf, len, "0x%02X", status);
    else        snprintf(buf, len, "0x00 (без ошибок)");
}

void faultText(char* buf, size_t len)
{
    switch (s_lastErr) {
    case Co2Err::NoAnswer: snprintf(buf, len, "нет связи"); break;
    case Co2Err::BadFrame: snprintf(buf, len, "сбой обмена"); break;
    case Co2Err::Sensor:   snprintf(buf, len, "ERR x%02X", s_errStatus); break;
    default:               snprintf(buf, len, "нет данных"); break;
    }
}

void statusText(char* buf, size_t len)
{
    char bits[ERRLOG_LINE_LEN / 2], fault[32];
    statusBitsText(s_errStatus, bits, sizeof(bits));
    faultText(fault, sizeof(fault));
    if (isValid(millis())) snprintf(buf, len, "OK, %u ppm, Error Status %s, ошибок обмена: %lu", s_co2, bits, (unsigned long)s_errors);
    else                   snprintf(buf, len, "%s, Error Status %s, ошибок обмена: %lu", fault, bits, (unsigned long)s_errors);
}

// ------------------------------------------------------------------ ABC
bool abcRequestRead()
{
    if (s_abcState == AbcState::Busy) return false;
    s_wantPeriod = -1;
    s_wantOn     = -1;
    s_abcMsg[0]  = '\0';
    s_abcState   = AbcState::Busy;
    s_abcNext    = Op::Map;
    return true;
}

bool abcRequestWrite(int32_t periodH, int8_t on)
{
    if (s_abcState == AbcState::Busy) return false;
    if (periodH < 0 && on < 0) return false;
    if (periodH >= 0 && (periodH < ABC_PERIOD_MIN_H || periodH > ABC_PERIOD_MAX_H)) return false;
    s_wantPeriod = periodH;
    s_wantOn     = on < 0 ? -1 : (on ? 1 : 0);
    s_abcMsg[0]  = '\0';
    s_abcState   = AbcState::Busy;
    s_abcNext    = Op::Map;
    return true;
}

AbcState    abcState()   { return s_abcState; }
uint16_t    abcPeriodH() { return s_period; }
bool        abcOn()      { return !(s_mc & K22_MC_ABC_DISABLE); }
const char* abcMessage() { return s_abcMsg; }

} // namespace Co2Sensor
#endif // CO2_SENSOR_K22
