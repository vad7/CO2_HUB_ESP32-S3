// ds18b20.cpp - драйвер DS18B20 на 1-Wire (вывод TEMP_PIN_DQ), один датчик на линии (Docs/DS18B20.pdf).
// Измерение: сброс -> SKIP ROM -> CONVERT T -> ожидание DS18B20_CONV_MS -> сброс -> SKIP ROM ->
// READ SCRATCHPAD -> 9 байт -> проверка CRC-8 (Dallas/Maxim, x^8+x^5+x^4+1) и регистра конфигурации;
// разрешение - биты R1 R0 конфигурации (табл. 3). Перед первым измерением - READ ROM (33h, один датчик на
// линии): код семейства, 48-бит серийный номер, CRC. Смена разрешения (requestResolution): перед измерением
// WRITE SCRATCHPAD (4Eh: TH, TL, конфигурация - все 3 байта) -> сброс -> SKIP ROM -> COPY SCRATCHPAD (48h) ->
// ожидание DS18B20_COPY_MS (запись в EEPROM, питание от VDD) - значение сохраняется и после выключения.
// Неблокирующий: за вызов poll() - один шаг (сброс ~0.55 мс или байт ~0.6 мс); восстановление линии
// после сброса и преобразование - по таймеру. Тайм-слоты битов (≤ 70 мкс) - в критической секции,
// чтобы прерывание не растянуло импульс. Линия - открытый сток (GPIO_MODE_INPUT_OUTPUT_OD), подтяжка
// 4.7 кОм внешняя (внутренняя ~45 кОм включена только как запасная).
#include "temp_backend.h"
#include "config.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <string.h>

namespace Ds18b20 {

// --- тайминги 1-Wire, мкс (стандартная скорость, Maxim AN126) ---
constexpr uint32_t OW_RESET_LOW_US    = 500;   // импульс сброса (мин. 480)
constexpr uint32_t OW_PRESENCE_US     = 70;    // от отпускания линии до проверки присутствия
constexpr uint32_t OW_RESET_REST_US   = 430;   // остаток окна присутствия (480 всего)
constexpr uint32_t OW_W1_LOW_US       = 6;     // запись 1: короткий низкий уровень
constexpr uint32_t OW_W1_REST_US      = 64;
constexpr uint32_t OW_W0_LOW_US       = 60;    // запись 0: низкий уровень на весь слот
constexpr uint32_t OW_W0_REST_US      = 10;
constexpr uint32_t OW_R_LOW_US        = 6;     // чтение: старт слота
constexpr uint32_t OW_R_SAMPLE_US     = 9;     // выборка через 15 мкс от начала слота
constexpr uint32_t OW_R_REST_US       = 55;
// --- DS18B20 ---
constexpr uint8_t  CMD_SKIP_ROM       = 0xCC;
constexpr uint8_t  CMD_CONVERT_T      = 0x44;
constexpr uint8_t  CMD_READ_SCRATCH   = 0xBE;
constexpr uint8_t  CMD_READ_ROM       = 0x33;
constexpr uint8_t  CMD_WRITE_SCRATCH  = 0x4E;
constexpr uint8_t  CMD_COPY_SCRATCH   = 0x48;
constexpr uint8_t  SCRATCH_TH         = 2;     // TH, TL - сохраняются при записи как есть
constexpr uint8_t  SCRATCH_TL         = 3;
constexpr uint8_t  SCRATCH_LEN        = 9;     // T LSB, T MSB, TH, TL, конфигурация, FF, резерв, 10h, CRC
constexpr uint8_t  SCRATCH_CFG        = 4;
constexpr uint8_t  CFG_FIXED_MASK     = 0x9F;  // конфигурация: бит 7 = 0, биты 4..0 = 1 (биты 6..5 - разрешение)
constexpr uint8_t  CFG_FIXED_BITS     = 0x1F;
constexpr uint8_t  CFG_RES_SHIFT      = 5;     // R0 - бит 5, R1 - бит 6 (рис. 7)
constexpr uint8_t  CFG_RES_MASK       = 0x03;
constexpr int16_t  RAW_POWER_ON       = 0x0550;   // +85 °C - значение после включения: CONVERT T запрошен, значит датчик
                                                 // перезапустился до чтения (просадка питания); в помещении 85 °C не бывает
constexpr uint8_t  CRC8_POLY          = 0x8C;  // x^8+x^5+x^4+1, отражённый
constexpr uint8_t  BITS_PER_BYTE      = 8;
constexpr int16_t  RAW_PER_DEG        = 16;    // 12 бит: 1/16 °C
constexpr int16_t  TENTHS             = 10;

enum class State : uint8_t { ResetRom, RomCmd, RomBytes,
                             ResetWrite, SkipWrite, WriteCmd, WriteTh, WriteTl, WriteCfg, ResetCopy, SkipCopy, CopyCmd, Copying,
                             ResetConv, SkipConv, Convert, Converting, ResetRead, SkipRead, ReadCmd, ReadBytes };

static const gpio_num_t PIN = (gpio_num_t)TEMP_PIN_DQ;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;  // критическая секция тайм-слотов 1-Wire

static State    s_state     = State::ResetConv;     // шаг автомата обмена
static uint32_t s_convStart = 0;                    // начало преобразования (millis)
static uint32_t s_readyUs   = 0;        // до этого момента линия восстанавливается после сброса
static uint8_t  s_buf[SCRATCH_LEN];                 // прочитанный scratchpad (9 байт)
static uint8_t  s_idx       = 0;                    // сколько байт scratchpad / ROM уже прочитано
static uint8_t  s_rom[DS18B20_ROM_LEN];             // ROM: код семейства, 48-бит серийный номер, CRC
static bool     s_romOk     = false;                // ROM прочитан и CRC верна
static uint8_t  s_resBits   = 0;        // разрешение из конфигурации (0 - ещё не читали)
static uint8_t  s_th = 0, s_tl = 0;     // TH, TL из последнего чтения (пишутся обратно вместе с конфигурацией)
static uint8_t  s_wantBits  = 0;        // записать разрешение (0 - не нужно)
static uint32_t s_copyStart = 0;                    // начало записи scratchpad в EEPROM (millis)

// ------------------------------------------------------------------ 1-Wire
static bool busReady() { return (int32_t)(micros() - s_readyUs) >= 0; }

// Сброс и проверка присутствия. Низкий уровень сброса - вне критической секции (растяжение не мешает).
static TempErr busReset()
{
    if (gpio_get_level(PIN) == 0) return TempErr::LineLow;   // линия замкнута или нет подтяжки
    gpio_set_level(PIN, 0);
    esp_rom_delay_us(OW_RESET_LOW_US);
    bool presence;
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(PIN, 1);
    esp_rom_delay_us(OW_PRESENCE_US);
    presence = gpio_get_level(PIN) == 0;
    portEXIT_CRITICAL(&s_mux);
    s_readyUs = micros() + OW_RESET_REST_US;
    return presence ? TempErr::None : TempErr::NoPresence;
}

static void writeBit(bool bit)
{
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(PIN, 0);
    esp_rom_delay_us(bit ? OW_W1_LOW_US : OW_W0_LOW_US);
    gpio_set_level(PIN, 1);
    esp_rom_delay_us(bit ? OW_W1_REST_US : OW_W0_REST_US);
    portEXIT_CRITICAL(&s_mux);
}

static bool readBit()
{
    bool bit;
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(PIN, 0);
    esp_rom_delay_us(OW_R_LOW_US);
    gpio_set_level(PIN, 1);
    esp_rom_delay_us(OW_R_SAMPLE_US);
    bit = gpio_get_level(PIN) != 0;
    esp_rom_delay_us(OW_R_REST_US);
    portEXIT_CRITICAL(&s_mux);
    return bit;
}

static void writeByte(uint8_t v)
{
    for (uint8_t i = 0; i < BITS_PER_BYTE; i++, v >>= 1) writeBit(v & 1);   // младший бит первым
}

static uint8_t readByte()
{
    uint8_t v = 0;
    for (uint8_t i = 0; i < BITS_PER_BYTE; i++) if (readBit()) v |= (uint8_t)(1 << i);
    return v;
}

static uint8_t crc8(const uint8_t* d, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        for (uint8_t k = 0; k < BITS_PER_BYTE; k++, b >>= 1) {
            const bool mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= CRC8_POLY;
        }
    }
    return crc;
}

static TempErr decode(int16_t& tenths)
{
    if (crc8(s_buf, SCRATCH_LEN - 1) != s_buf[SCRATCH_LEN - 1]) return TempErr::Crc;
    if ((s_buf[SCRATCH_CFG] & CFG_FIXED_MASK) != CFG_FIXED_BITS) return TempErr::BadData;   // нули / мусор с верной CRC
    s_resBits = (uint8_t)(DS18B20_RES_MIN_BITS + ((s_buf[SCRATCH_CFG] >> CFG_RES_SHIFT) & CFG_RES_MASK));
    s_th = s_buf[SCRATCH_TH];
    s_tl = s_buf[SCRATCH_TL];
    // при меньшем разрешении младшие биты - нули (раздел Operation): формула та же
    const int16_t raw = (int16_t)(((uint16_t)s_buf[1] << BITS_PER_BYTE) | s_buf[0]);
    if (raw == RAW_POWER_ON) return TempErr::PowerOn;
    tenths = (int16_t)(((int32_t)raw * TENTHS + (raw >= 0 ? RAW_PER_DEG / 2 : -RAW_PER_DEG / 2)) / RAW_PER_DEG);
    return TempErr::None;
}

// ------------------------------------------------------------------ интерфейс драйвера
void attach()
{
    if (TEMP_PIN_DQ < 0) return;
    gpio_reset_pin(PIN);
    gpio_set_direction(PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(PIN, GPIO_PULLUP_ONLY);
    gpio_set_level(PIN, 1);
    s_readyUs = micros();
    s_romOk = false;           // датчик могли заменить - номер и разрешение читаются заново
    s_resBits = s_wantBits = 0;
}

void detach()
{
    if (TEMP_PIN_DQ >= 0) gpio_reset_pin(PIN);   // вход с подтяжкой - как после включения
}

// Перед измерением: серийный номер (пока не прочитан), запись разрешения (если запрошена и TH / TL уже известны)
static State firstState()
{
    if (!s_romOk) return State::ResetRom;
    if (s_wantBits && s_resBits) return State::ResetWrite;
    return State::ResetConv;
}

void start(uint32_t) { s_state = firstState(); }

bool serial(uint8_t* rom)
{
    memcpy(rom, s_rom, DS18B20_ROM_LEN);
    return s_romOk;
}

uint8_t resolutionBits() { return s_resBits; }

void requestResolution(uint8_t bits)
{
    if (bits >= DS18B20_RES_MIN_BITS && bits <= DS18B20_RES_MAX_BITS) s_wantBits = bits;
}

TempPoll poll(uint32_t now, TempErr& err, int16_t& tenths, int16_t& rhTenths)
{
    rhTenths = -1;   // только температура
    if (!busReady()) return TempPoll::Busy;
    switch (s_state) {
    case State::ResetRom:
        if ((err = busReset()) != TempErr::None) return TempPoll::Done;
        s_state = State::RomCmd;
        break;
    case State::RomCmd:    writeByte(CMD_READ_ROM); s_idx = 0; s_state = State::RomBytes; break;
    case State::RomBytes:
        s_rom[s_idx++] = readByte();
        if (s_idx < DS18B20_ROM_LEN) break;
        s_romOk = crc8(s_rom, DS18B20_ROM_LEN - 1) == s_rom[DS18B20_ROM_LEN - 1];   // нет - повтор в следующем цикле
        s_state = (s_wantBits && s_resBits) ? State::ResetWrite : State::ResetConv;
        break;
    case State::ResetWrite:
        if ((err = busReset()) != TempErr::None) return TempPoll::Done;
        s_state = State::SkipWrite;
        break;
    case State::SkipWrite: writeByte(CMD_SKIP_ROM);      s_state = State::WriteCmd; break;
    case State::WriteCmd:  writeByte(CMD_WRITE_SCRATCH); s_state = State::WriteTh; break;
    case State::WriteTh:   writeByte(s_th);              s_state = State::WriteTl; break;
    case State::WriteTl:   writeByte(s_tl);              s_state = State::WriteCfg; break;
    case State::WriteCfg:
        writeByte((uint8_t)(((s_wantBits - DS18B20_RES_MIN_BITS) << CFG_RES_SHIFT) | CFG_FIXED_BITS));
        s_state = State::ResetCopy;
        break;
    case State::ResetCopy:
        if ((err = busReset()) != TempErr::None) return TempPoll::Done;
        s_state = State::SkipCopy;
        break;
    case State::SkipCopy:  writeByte(CMD_SKIP_ROM);     s_state = State::CopyCmd; break;
    case State::CopyCmd:   writeByte(CMD_COPY_SCRATCH); s_copyStart = now; s_state = State::Copying; break;
    case State::Copying:
        if (now - s_copyStart < DS18B20_COPY_MS) break;
        s_wantBits = 0;                                  // разрешение перечитается в этом же измерении
        s_state = State::ResetConv;
        break;
    case State::ResetConv:
        if ((err = busReset()) != TempErr::None) return TempPoll::Done;
        s_state = State::SkipConv;
        break;
    case State::SkipConv:  writeByte(CMD_SKIP_ROM);  s_state = State::Convert; break;
    case State::Convert:   writeByte(CMD_CONVERT_T); s_convStart = now; s_state = State::Converting; break;
    case State::Converting:
        if (now - s_convStart >= DS18B20_CONV_MS) s_state = State::ResetRead;
        break;
    case State::ResetRead:
        if ((err = busReset()) != TempErr::None) return TempPoll::Done;
        s_state = State::SkipRead;
        break;
    case State::SkipRead:  writeByte(CMD_SKIP_ROM);     s_state = State::ReadCmd; break;
    case State::ReadCmd:   writeByte(CMD_READ_SCRATCH); s_idx = 0; s_state = State::ReadBytes; break;
    case State::ReadBytes:
        s_buf[s_idx++] = readByte();
        if (s_idx < SCRATCH_LEN) break;
        err = decode(tenths);
        return TempPoll::Done;
    }
    return TempPoll::Busy;
}

} // namespace Ds18b20
