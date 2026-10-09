// config.h - выбор платы и датчика CO2, жёсткая карта пинов и константы проекта.
// Проект: датчик CO2 + хаб вентиляторов (SenseAir K22-OC по I2C или S8 LP по UART, nRF24L01+, Wi-Fi), см. Readme.md
//
// Fuses: нет. ESP32-S3: eFuse не программируются; тип Flash/PSRAM задаётся в platformio.ini.
//
// Плата выбирается окружением PlatformIO (-DBOARD_ES3C28P или -DBOARD_TDISPLAY_S3).
// Распиновка взята из документации в Docs/ - менять только по схеме платы!
#pragma once
#include <stdint.h>
#include <time.h>

#if defined(BOARD_ES3C28P) && defined(BOARD_TDISPLAY_S3)
#error "Выберите только одну плату: BOARD_ES3C28P или BOARD_TDISPLAY_S3"
#endif

// =====================================================================================
// Датчик CO2: выбор здесь (или -DCO2_SENSOR=... в build_flags platformio.ini)
// =====================================================================================
#define CO2_SENSOR_K22          1       // SenseAir K22-OC, I2C (Docs/SensAir_I2C_comm_guide_2_1031.pdf)
#define CO2_SENSOR_S8           2       // SenseAir S8 LP 004-0-0053, UART Modbus RTU (Docs/Senseair S8 *.md)
#ifndef CO2_SENSOR
#define CO2_SENSOR              CO2_SENSOR_K22
#endif
#if CO2_SENSOR == CO2_SENSOR_K22
#define CO2_SENSOR_NAME         "K22-OC"
#define FW_SENSOR_ID            "k22"           // в имени файла прошивки firmware_<плата>_<датчик>_<версия>.bin
#elif CO2_SENSOR == CO2_SENSOR_S8
#define CO2_SENSOR_NAME         "S8 LP"
#define FW_SENSOR_ID            "s8"
#else
#error "CO2_SENSOR: CO2_SENSOR_K22 или CO2_SENSOR_S8"
#endif

// =====================================================================================
// Датчик температуры (необязательный) - выбирается в настройках (меню / веб): нет, DS18B20 или SHT40.
//   DS18B20 (1-Wire): DQ - вывод TEMP_PIN_DQ (ниже, у платы; -1 - вывода нет, DS18B20 не предлагается)
//            + подтяжка 4.7 кОм DQ -> 3.3 В; VDD - 3.3 В (не паразитное питание); GND общий. Один датчик на линии.
//   SHT40 (I2C, Sensirion SHT4x): на шине I2C датчика K22 / тача (I2C_PIN_SDA / I2C_PIN_SCL ниже), 3.3 В, GND общий;
//            адрес 0x44 (варианты 0x45 / 0x46 - ищутся автоматически). Влажность не используется.
// =====================================================================================

// =====================================================================================
#if defined(BOARD_ES3C28P)
// ESP32-S3 Ai Xiaozhi ES3C28P, Docs/2.8inch_ESP32-S3_Display_Schematic.pdf
// =====================================================================================
#define BOARD_NAME              "ES3C28P"
#define FW_BOARD_ID             "es3c28p"       // = env platformio.ini: имя файла прошивки firmware_<id>_<версия>.bin

// --- Дисплей ILI9341V 240x320, SPI2 (FSPI), IO_MUX-пины ---
#define LCD_PIN_CS              10
#define LCD_PIN_MOSI            11
#define LCD_PIN_SCK             12
#define LCD_PIN_MISO            13
#define LCD_PIN_DC              46      // LCD_RS. Strapping-пин
#define LCD_PIN_RST             -1      // Сброс LCD = CHIP_PU (общий с ESP32)
#define LCD_PIN_BL              45      // HIGH = подсветка вкл. Strapping-пин (на плате 10k к GND)
#define LCD_WIDTH               240
#define LCD_HEIGHT              320
#define LCD_OFFSET_X            0
#define LCD_OFFSET_Y            0
#define LCD_INVERT              true    // IPS-матрица. Если цвета негативные - поменять на false
#define LCD_SPI_HZ              40000000
#define LCD_ROTATION            1       // Альбомная ориентация 320x240

// --- Сенсорный экран FT6336G (I2C0, общий с ES8311 и разъёмом P4) ---
#define HAS_TOUCH               1
#define TOUCH_PIN_INT           17
#define TOUCH_PIN_RST           18
#define TOUCH_I2C_ADDR          0x38
#define TOUCH_REG_VENDOR_ID     0xA8    // FT6336G: регистр ID производителя - проверка наличия тача

// --- Шина I2C0: тач + K22 (разъём P4: 1-3V3, 2-GND, 3-SCL, 4-SDA), подтяжки 4.7k на плате ---
#define I2C_PORT                0
#define I2C_PIN_SDA             16
#define I2C_PIN_SCL             15

// --- nRF24L01+ на SPI3 (HSPI) через GPIO-matrix ---
// P3: 1-IO2, 2-IO3, 3-IO14, 4-IO21;  P2: 1-+5V, 2-GND, 3-TXD0(GPIO43), 4-RXD0(GPIO44)
#define NRF_PIN_SCK             14      // P3.3
#define NRF_PIN_MOSI            21      // P3.4
#define NRF_PIN_MISO            3       // P3.2 (strapping JTAG-sel, вход ESP32 - безопасно)
#define NRF_PIN_CSN             44      // P2.4 RXD0 (Serial идёт через USB-CDC)
// CE модуля - перемычкой на 3.3 В (не управляется ни на одной плате, см. radio.cpp); GPIO43 - под UART S8
#define NRF_SPI_BUS             HSPI    // SPI3: SPI2 (FSPI) занят дисплеем

// --- SenseAir S8 LP, UART1 через GPIO-matrix (уровни 3.3 В - напрямую) ---
#define S8_PIN_TX               43      // P2.3 TXD0 -> S8 UART_RxD (последовательно 499R+100R на плате)
#define S8_PIN_RX               2       // P3.1 IO2  <- S8 UART_TxD (свободный IO по руководству платы)

// --- DS18B20 DQ: свободный IO2 разъёма P3 - только с K22 (с S8 там UART_TxD датчика) ---
#if CO2_SENSOR == CO2_SENSOR_S8
// ES3C28P + S8: свободного вывода нет (IO2 и TXD0 - UART S8, остальные IO разъёмов - nRF24 и I2C):
// DS18B20 в настройках не предлагается; SHT40 - на разъёме P4 (I2C)
#define TEMP_PIN_DQ             -1
#else
#define TEMP_PIN_DQ             2       // P3.1 (IO2); 4.7k к 3V3 (P4.1)
#endif

// --- Кнопки: BOOT (KEY2) ---
#define BTN_PIN_A               0       // BOOT, активный LOW, подтяжка 10k на плате
#define BTN_PIN_B               -1      // нет второй кнопки

// =====================================================================================
#elif defined(BOARD_TDISPLAY_S3)
// LILYGO T-Display-S3 1.9", Docs/LILYGO T-Display ESP32-S3.png и *Pinout.docx
// =====================================================================================
#define BOARD_NAME              "T-Display-S3"
#define FW_BOARD_ID             "tdisplay_s3"   // = env platformio.ini: имя файла прошивки firmware_<id>_<версия>.bin

// --- Дисплей ST7789 170x320, 8-бит параллельная шина (LCD_CAM) ---
#define LCD_PIN_D0              39
#define LCD_PIN_D1              40
#define LCD_PIN_D2              41
#define LCD_PIN_D3              42
#define LCD_PIN_D4              45
#define LCD_PIN_D5              46
#define LCD_PIN_D6              47
#define LCD_PIN_D7              48
#define LCD_PIN_WR              8
#define LCD_PIN_RD              9
#define LCD_PIN_DC              7
#define LCD_PIN_CS              6
#define LCD_PIN_RST             5
#define LCD_PIN_BL              38
#define LCD_PIN_POWER_ON        15      // HIGH = питание LCD (и работа от батареи)
#define LCD_WIDTH               170
#define LCD_HEIGHT              320
#define LCD_OFFSET_X            35      // 170 из 240 колонок ST7789 - проверить на железе
#define LCD_OFFSET_Y            0
#define LCD_INVERT              true
#define LCD_BUS_HZ              20000000
#define LCD_ROTATION            1       // Альбомная ориентация 320x170

#define HAS_TOUCH               0       // Обычная версия без тача: настройка кнопками

// --- I2C для K22: штатные SDA/SCL платы на разъёме P1 ---
#define I2C_PORT                0
#define I2C_PIN_SDA             18      // P1
#define I2C_PIN_SCL             17      // P1

// --- nRF24L01+ на SPI2 (FSPI) IO_MUX-пины левого разъёма P2 (дисплей параллельный) ---
#define NRF_PIN_SCK             12
#define NRF_PIN_MOSI            11
#define NRF_PIN_MISO            13
#define NRF_PIN_CSN             10
// CE модуля - перемычкой на 3.3 В (не управляется, см. radio.cpp); GPIO1 (P2.2) свободен
#define NRF_SPI_BUS             FSPI    // SPI2 свободен (дисплей на LCD_CAM)

// --- SenseAir S8 LP, UART1 через GPIO-matrix: правый разъём P1 (U0TXD/U0RXD, Serial - через USB-CDC) ---
#define S8_PIN_TX               43      // P1 GPIO43 -> S8 UART_RxD
#define S8_PIN_RX               44      // P1 GPIO44 <- S8 UART_TxD

// --- DS18B20 DQ: GPIO1 левого разъёма P2 (свободен: CE nRF24 на перемычке) ---
#define TEMP_PIN_DQ             1       // P2.2 (GPIO1); 4.7k к 3V (P2.1)

// --- Кнопки на плате ---
#define BTN_PIN_A               0       // BOOT
#define BTN_PIN_B               14      // IO14

#else
#error "Не выбрана плата: задайте -DBOARD_ES3C28P или -DBOARD_TDISPLAY_S3 (env в platformio.ini)"
#endif

// =====================================================================================
// Общие константы
// =====================================================================================

// --- Датчик CO2: опрос - одинаковый для K22 и S8, период - настройка co2.co2PollS ---
constexpr uint8_t  CO2_POLL_DEF_S        = 2;       // по умолчанию (K22 меряет раз в 2 с, S8 - раз в 4 с: читается то же
                                                    // значение, зато статус S8 - раз в 2 с, как рекомендовано)
constexpr uint8_t  CO2_POLL_MIN_S        = 1;       // настройка 1..60 с
constexpr uint8_t  CO2_POLL_MAX_S        = 60;
constexpr uint8_t  CO2_FAIL_PERIODS      = 3;       // нет валидных данных дольше 3 периодов опроса - «нет датчика»

// --- SenseAir K22 (Docs/SensAir_I2C_comm_guide_2_1031.pdf) ---
constexpr uint8_t  K22_I2C_ADDR          = 0x68;    // адрес по умолчанию
constexpr uint32_t K22_I2C_HZ            = 100000;  // максимум для K2x
// Команда - старший полубайт 1-го байта запроса, младший - число байт данных (табл. 7)
constexpr uint8_t  K22_CMD_WRITE_RAM     = 0x1;
constexpr uint8_t  K22_CMD_READ_RAM      = 0x2;
constexpr uint8_t  K22_CMD_WRITE_EE      = 0x3;     // EEPROM: K22-OC - есть, 128 байт (табл. 10)
constexpr uint8_t  K22_CMD_READ_EE       = 0x4;
constexpr uint16_t K22_RAM_CO2           = 0x0008;  // Space CO2, 2 байта, старший первым (табл. 11)
constexpr uint16_t K22_RAM_MEMMAP        = 0x002F;  // идентификатор карты памяти, 1 байт (табл. 14)
constexpr uint8_t  K22_MEMMAP_A          = 0x09;    // карты K22, для которых даны адреса ниже (табл. 12, 16)
constexpr uint8_t  K22_MEMMAP_B          = 0x0A;
constexpr uint16_t K22_EE_METER_CONTROL  = 0x003E;  // MeterControl в EEPROM (табл. 12); смена - после перезапуска питания
constexpr uint8_t  K22_MC_ABC_DISABLE    = 0x02;    //   бит 1: 1 - ABC выключен, 0 - включён (табл. 16)
constexpr uint16_t K22_EE_ABC_PERIOD     = 0x0040;  // ABC Period в EEPROM, 2 байта, старший первым, часы (табл. 16)
constexpr uint32_t K22_WAIT_MS           = 20;      // tWAIT между запросом и ответом (тип. 20 мс)
constexpr uint8_t  K22_READ_RETRIES      = 5;       // повторы чтения, если ответ «не готов»

// --- SenseAir S8 LP, Modbus RTU (Docs/Senseair S8 Modbus.md) ---
constexpr uint32_t S8_BAUD               = 9600;    // только 9600, 8N1 (датчик передаёт 2 стоп-бита - 8N1 принимает)
constexpr uint8_t  S8_MODBUS_ADDR        = 0xFE;    // «любой датчик» (один датчик на линии)
constexpr uint8_t  S8_FUNC_READ_INPUT    = 0x04;    // Read Input Registers
constexpr uint8_t  S8_FUNC_READ_HOLDING  = 0x03;    // Read Holding Registers
constexpr uint8_t  S8_FUNC_WRITE_SINGLE  = 0x06;    // Write Single Register (ответ - эхо запроса)
constexpr uint16_t S8_HR_ABC_PERIOD      = 0x001F;  // HR32 ABC Period, часы; 0 - ABC приостановлен
constexpr uint8_t  S8_EXCEPTION_FLAG     = 0x80;    // функция + 0x80 - ответ-исключение
constexpr uint16_t S8_REG_FIRST          = 0x0000;  // IR1 MeterStatus ...
constexpr uint16_t S8_REG_COUNT          = 4;       // ... по IR4 Space CO2 одним запросом
constexpr uint8_t  S8_REG_IDX_STATUS     = 0;       // IR1 - индекс в ответе
constexpr uint8_t  S8_REG_IDX_CO2        = 3;       // IR4
constexpr uint16_t S8_STATUS_ERRORS      = 0x005F;  // MeterStatus: Fatal, Offset, Algorithm, Output, Self-diag, Memory
                                                    // (Out of range 0x20 - показание есть, не считаем отказом)
constexpr uint32_t S8_RESPONSE_TIMEOUT_MS = 250;    // по спецификации ответ не позже 180 мс

// --- ABC (Automatic Baseline Correction) датчика CO2: чтение / запись из веба ---
constexpr uint16_t ABC_PERIOD_MIN_H      = 1;       // период, ч (регистр - 16 бит без знака: 1..65535)
constexpr uint16_t ABC_PERIOD_MAX_H      = UINT16_MAX;
constexpr uint8_t  ABC_MSG_LEN           = 96;      // строка состояния обмена (ошибка / примечание)

// --- Датчик температуры: DS18B20 (1-Wire, SKIP ROM - один датчик на линии) или SHT40 (I2C) ---
constexpr bool     TEMP_DS18B20_AVAILABLE = TEMP_PIN_DQ >= 0;   // есть вывод под DS18B20
constexpr uint8_t  TEMP_PERIOD_DEF_S     = 10;      // период чтения по умолчанию (настройка 1..60 с)
constexpr uint8_t  TEMP_PERIOD_MIN_S     = 1;
constexpr uint8_t  TEMP_PERIOD_MAX_S     = 60;
constexpr uint32_t DS18B20_CONV_MS       = 750;     // tCONV max при 12 битах (по умолчанию после включения)
constexpr uint8_t  TEMP_FAIL_COUNT       = 3;       // ошибок подряд - значение считается недостоверным
constexpr uint8_t  SHT40_ADDRS[]         = { 0x44, 0x45, 0x46 };   // SHT40-AD1B / BD1B / CD1B (даташит SHT4x)
constexpr uint8_t  SHT40_CMD_MEASURE_HI  = 0xFD;    // измерение T и RH, высокая точность
constexpr uint32_t SHT40_MEASURE_MS      = 10;      // длительность измерения: макс. 8.2 мс + запас

// --- Радио nRF24L01+ (протокол хаба ESP8266 -> вентиляторы, Old/ESP8266_WIFI/app/nrf24l01.c) ---
constexpr uint32_t RADIO_SPI_HZ          = 4000000;
constexpr uint8_t  RADIO_ADDR_WIDTH      = 3;
constexpr uint8_t  RADIO_ADDR_BASE1      = 0xC8;    // старшие байты адреса
constexpr uint8_t  RADIO_ADDR_BASE2      = 0xC8;
constexpr uint8_t  RADIO_RETRY_DELAY     = 2;       // ARD = (2+1)*250 = 750 мкс
constexpr uint8_t  RADIO_RETRY_COUNT     = 15;      // ARC
constexpr uint8_t  RADIO_PAYLOAD_LEN     = 4;
constexpr uint32_t RADIO_TX_TIMEOUT_MS   = 50;      // хаб: 50 опросов по 1 мс

// --- Пассивный режим связи (Old/CO2UART: вентиляторы сами запрашивают хаб, ответ - Payload with ACK) ---
constexpr uint8_t  RADIO_PIPES_MAX       = 6;       // каналов приёма nRF24: вентилятор N - канал (pipe) N
constexpr uint8_t  RADIO_ACK_FIFO        = 3;       // TX FIFO nRF24: столько ответов в ACK может ждать одновременно
constexpr uint8_t  PASSIVE_FANS_MAX      = RADIO_ACK_FIFO;   // вентиляторов в пассивном режиме: ответ готов каждому
static_assert(PASSIVE_FANS_MAX <= RADIO_PIPES_MAX, "каналов приёма nRF24 - не больше 6");
constexpr uint8_t  RADIO_MAX_PAYLOAD     = 32;      // максимальная длина пакета nRF24
constexpr uint8_t  PASSIVE_CHANNEL_DEF   = 120;     // sensor_rf_channel старого хаба CO2UART
constexpr uint16_t RADIO_RESET_DEF_S     = 3600;    // nRF24_reset_time: нет приёма дольше - перезапуск модуля (0 - нет)
constexpr uint8_t  FAN_PAUSE_DEF_S       = 10;      // пауза вентилятора до следующего запроса (передаётся ему в ACK)
constexpr uint8_t  FAN_TIMEOUT_DEF_S     = 60;      // вентилятор молчит дольше - «нет связи» (0 - не проверять)
// Байт состояния от вентилятора (CO2UART: user_loop)
constexpr uint8_t  FAN_ST_EEPROM_BROKEN  = 0xEE;    // сбой ячейки EEPROM вентилятора
constexpr uint8_t  FAN_ST_TX_SHIFT       = 5;       // биты 7..5 - статус связи вентилятора (0 - ok)
constexpr uint8_t  FAN_ST_TX_MASK        = 0x07;
constexpr uint8_t  FAN_ST_OFF_BIT        = 0x10;    // бит 4 - вентилятор выключен
constexpr uint8_t  FAN_ST_ADJ_MASK       = 0x0F;    // биты 3..0 - поправка скорости со знаком (−8..+7)
constexpr uint8_t  FAN_ST_ADJ_SIGN       = 0x08;

// --- Логика хаба (Old/ESP8266_WIFI/app/wireless_co2.c) ---
constexpr uint8_t  FANS_MAX              = 10;      // FANS_MAX хаба
constexpr int8_t   FAN_SPEED_MAX         = 6;
constexpr uint8_t  CO2_AVERAGE_LEN       = 6;       // CO2LevelAverageArrayLength хаба
constexpr uint8_t  FAN_NAME_LEN          = 32;      // имя вентилятора, байт UTF-8 (32 латинских или 16 русских букв)
constexpr uint16_t TRANSMIT_PERIOD_DEF_S = 20;      // период рассылки 
constexpr uint16_t TRANSMIT_PERIOD_MIN_S = 2;
constexpr uint16_t TRANSMIT_PERIOD_MAX_S = 3600;
constexpr uint32_t FIRST_TRANSMIT_MS     = 5000;    // первая рассылка после старта
constexpr uint32_t FAN_TICK_MS           = 1000;    // user_loop хаба: раз в 1 с (таймауты принуд. скорости)
constexpr uint16_t FORCE_MINUTES_DEF     = 180;     // время коррекции по умолчанию (cookie fsp_time хаба)
constexpr uint8_t  RF_CHANNEL_MAX        = 125;
constexpr uint8_t  RF_CHANNEL_DEF        = 2;       // sensor_rf_channel хаба по умолчанию
constexpr uint8_t  FAN_ADDR_LSB_DEF      = 0xC1;    // адрес нового вентилятора: C1 + номер
// Умолчания настроек CO2 (hub_config.cpp, defaultsCo2): пороги скорости 1..6, ppm - wireless_co2_init() хаба
constexpr uint16_t CO2_THRESHOLDS_DEF[FAN_SPEED_MAX] = { 500, 550, 600, 800, 900, 1100 };
constexpr uint16_t NIGHT_START_DEF       = 2200;    // ночь в будни: начало, hhmm
constexpr uint16_t NIGHT_END_DEF         = 600;     //   конец
constexpr uint16_t NIGHT_START_WD_DEF    = 2300;    // ночь в выходные (сб, вс): начало, hhmm
constexpr uint16_t NIGHT_END_WD_DEF      = 800;     //   конец

// --- История CO2 и температуры (кольцевые буферы в PSRAM; без PSRAM - во внутренней RAM) ---
// Размер задаётся в сутках: записей = сутки * 86400 / период рассылки; не больше, чем помещается в память.
constexpr uint16_t HISTORY_DAYS_DEF      = 7;
constexpr uint16_t HISTORY_DAYS_MIN      = 1;
constexpr uint16_t HISTORY_DAYS_MAX      = 365;
constexpr uint32_t HISTORY_RECORDS_MIN   = 100;     // меньше не выделяется (даже если просят меньше)
constexpr uint32_t HISTORY_CO2_REC_BYTES = 6;       // запись CO2: аптайм 4 + ppm 2
constexpr uint32_t HISTORY_TEMP_REC_BYTES = 2;      // запись температуры: 0.1 °C, int16
constexpr size_t   HISTORY_PSRAM_RESERVE = 64 * 1024;   // оставить свободным в PSRAM
constexpr size_t   HISTORY_RAM_RESERVE   = 96 * 1024;   // без PSRAM: оставить во внутренней RAM (Wi-Fi, веб)
constexpr uint32_t S_PER_DAY_CFG         = 86400;

#define FW_VERSION               "2.0.0"                // прошивка датчика-хаба (1.x - ESP8266 хаб)
#define FW_ID                    FW_BOARD_ID "_" FW_SENSOR_ID   // сборка: плата_датчик (es3c28p_k22, tdisplay_s3_s8 …)
#define FW_ID_MARKER             "CO2HUB_FWID="         // метка в образе: Scripts/copy_firmware.py берёт FW_ID из .bin
#define FW_AUTHOR                "Вадим (vad7@yahoo.com)"   // меню «О программе», веб «Система»
// Дата сборки FW_BUILD_DATE - в build_info.h (генерируется Scripts/build_info.py при каждой сборке)

// --- Время / сеть ---
constexpr time_t   TIME_VALID_EPOCH      = 1700000000;   // время до этой даты = NTP ещё не синхронизирован
#define NET_TZ_DEF               "MSK-3"                // POSIX TZ, меняется в веб-интерфейсе
#define NET_NTP_DEF              "pool.ntp.org"
#define NET_AP_SSID_DEF          "CO2-Hub"               // точка доступа для первичной настройки (меняется в вебе)
#define NET_AP_PASS_DEF          "co2sensor"             //   пароль по умолчанию (меняется в вебе)
constexpr size_t   NET_AP_PASS_MIN       = 8;       // WPA2: пароль точки доступа не короче
// Режим Wi-Fi (net.wifiMode, только меню экрана; по умолчанию - выкл.): режим «Часы» - Wi-Fi только на время
// синхронизации NTP: окно не дольше WIFI_CLOCK_WINDOW_MS, удача - следующее через net.ntpPeriodMin,
// неудача (нет роутера / NTP) - через WIFI_CLOCK_RETRY_MS
constexpr uint32_t WIFI_CLOCK_WINDOW_MS  = 60000;   // подключение к роутеру + ответ NTP
constexpr uint32_t WIFI_CLOCK_RETRY_MS   = 600000;  // повтор после неудачи, 10 мин
#define NET_WEB_PASS_DEF         "co2sensor"             // пароль настроек веба (меняется в вебе, пустой - без пароля; сброс - меню экрана)
#define NET_WEB_REALM            "CO2-Hub settings"      // HTTP Basic: имя области (только ASCII)
#define NET_HOSTNAME             "co2"                   // http://co2.local
constexpr uint16_t WIFI_AP_DELAY_DEF_MIN = 10;      // нет связи с роутером дольше -> AP+STA (настройка в вебе)
constexpr uint16_t WIFI_AP_DELAY_LO_MIN  = 1;
constexpr uint16_t WIFI_AP_DELAY_HI_MIN  = 1440;    // сутки
constexpr uint16_t NTP_PERIOD_DEF_MIN    = 1440;    // период обновления времени по NTP - раз в сутки;
constexpr uint16_t NTP_PERIOD_LO_MIN     = 1;       //   при старте синхронизация - всегда
constexpr uint16_t NTP_PERIOD_HI_MIN     = 10080;   //   неделя
constexpr uint32_t WIFI_RETRY_MS         = 60000;   // поиск роутера, пока связи нет (не при клиентах на AP)
constexpr uint32_t WIFI_AP_CHECK_MS      = 1000;    // проверка: подключён ли кто-то к точке доступа
constexpr uint32_t MS_PER_MIN            = 60000;
constexpr uint16_t WEB_REFRESH_DEF_MS    = 5000;    // page_refresh_time хаба
constexpr size_t   WEB_POST_MAX          = 2048;
constexpr size_t   WEB_OUT_BUF           = 1024;
constexpr size_t   WEB_JSON_MAX          = 2560;    // одна группа /api/vars; худшая - cpu (32 задачи) 2,25 КБ: Scripts/web_vars_size.py
constexpr char     HISTORY_CSV_SEP       = ';';     // history.csv: разделитель колонок (как в Excel с русской локалью)
constexpr char     HISTORY_CSV_DECIMAL   = ',';     //   и дробной части температуры
constexpr uint32_t WEB_STACK_SIZE        = 8192;

// --- Главный цикл ---
constexpr uint32_t LOOP_YIELD_TICKS      = 1;       // в конце loop() уступить ядро на 1 тик FreeRTOS (1 мс при 1000 Гц):
                                                    // проход не чаще 1000 раз/с - с запасом для всех автоматов
constexpr uint32_t LOOP_STAT_PERIOD_MS   = 10000;   // окно замера: в конце - максимумы за окно
constexpr uint32_t LOOP_SLOW_MS          = 50;      // печатать, только если самый долгий проход за окно дольше

// --- Состояние системы (sys_info.cpp) ---
constexpr uint32_t SYSINFO_PERIOD_MS     = 2000;    // снимок задач FreeRTOS, загрузка ядер, температура кристалла
constexpr uint8_t  SYSINFO_TASKS_MAX     = 32;      // задач в таблице (сейчас ~15-20; больше - показываются не все)
constexpr uint32_t TASK_STACK_WARN_B     = 512;     // мин. свободный стек задачи меньше - красным (экран, веб)
constexpr uint8_t  SYSINFO_TASK_NAME_LEN = 16;      // = configMAX_TASK_NAME_LEN (CONFIG_FREERTOS_MAX_TASK_NAME_LEN)

// --- Интерфейс ---
constexpr uint32_t UI_REFRESH_MS         = 250;
constexpr uint32_t UI_SETUP_TIMEOUT_MS   = 30000;   // выход из настроек при бездействии
constexpr uint32_t UI_SYSTEM_TIMEOUT_MS  = 60UL * 60 * 1000;   // группа «Система» (состояние, задачи, сеть - смотрят подолгу): выход через 60 мин
constexpr uint32_t UI_HISTORY_TIMEOUT_MS = 30000;   // возврат с графика истории на главный экран
constexpr uint32_t UI_MAIN_BUTTONS_MS    = 10000;   // кнопки главного экрана скрываются без касаний
constexpr uint8_t  TOUCH_CAL_MARK_SIZE   = 14;      // размер отметки калибровки в углу, px
constexpr uint32_t TOUCH_CAL_DONE_MS     = 1000;    // показ «Сохранено» после калибровки (setup)
constexpr uint32_t WEB_RESTART_DELAY_MS  = 500;     // перезапуск по команде из веба (после ответа)
constexpr uint32_t BTN_DEBOUNCE_MS       = 30;
constexpr uint32_t BTN_LONG_MS           = 800;
constexpr uint32_t TOUCH_POLL_MS         = 30;
constexpr uint16_t CO2_LEVEL_GREEN       = 500;     // цвета как на веб-странице хаба:
constexpr uint16_t CO2_LEVEL_NORMAL      = 700;     // <=500 зелёный, <=700 обычный,
constexpr uint16_t CO2_LEVEL_ORANGE      = 900;     // <=900 оранжевый, иначе красный
constexpr uint8_t  BRIGHT_DAY_DEF_PCT    = 80;      // яркость подсветки днём по умолчанию, %
constexpr uint8_t  BRIGHT_NIGHT_DEF_PCT  = 10;      // яркость подсветки ночью по умолчанию, %
constexpr uint8_t  BRIGHT_MIN_PCT        = 5;       // минимум (0 % = чёрный экран, меню не увидеть)
constexpr uint8_t  BRIGHT_MAX_PCT        = 100;
constexpr uint8_t  BRIGHT_STEP_PCT       = 5;       // шаг в тач-меню
constexpr uint8_t  LCD_BL_MAX            = 255;     // шкала ШИМ подсветки LovyanGFX
constexpr uint32_t SERIAL_BAUD           = 115200;
constexpr uint32_t SERIAL_WAIT_MS        = 1500;    // ожидание USB-CDC в setup()
constexpr uint32_t SERIAL_TX_TIMEOUT_MS  = 0;       // USB-CDC: не ждать отправки - без открытого терминала вывод
                                                    // отбрасывается, а не тормозит loop() (касания срабатывали через ~2 с)
