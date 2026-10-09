// net.h - Wi-Fi (STA, резервная точка доступа), NTP-время, mDNS
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

namespace Net {
    enum class Mode : uint8_t { Off, Connecting, Station, AccessPoint, StationAndAp };
    enum class TimeSource : uint8_t { None, Ntp, Manual };   // кто последним установил часы

    void     begin();
    void     update(uint32_t now);              // из loop()
    void     requestReconnect();                // после смены настроек (из веб-задачи)
    Mode     mode();
    bool     apActive();                        // точка доступа включена (AccessPoint или StationAndAp)
    bool     staEnabled();                      // SSID роутера задан
    bool     staConnected();                    // есть связь с роутером
    bool     wifiOn();                          // режим «вкл» (станция / точка доступа, веб) - применённый
    uint8_t  wifiMode();                        // WifiMode, применённый (net.wifiMode после startStation)
    bool     clockWindow();                     // режим «Часы»: окно синхронизации открыто (радио включено)
    uint32_t clockLastOkUpS();                  // режим «Часы»: аптайм последнего получения времени, 0 - не было
    uint32_t clockNextS();                      // режим «Часы»: секунд до следующего окна (0 - идёт или не режим)
    const char* staSsid();                      // SSID роутера (только из loop())
    const char* apSsid();                       // имя и пароль точки доступа (применённые; только из loop())
    const char* apPass();
    void     apIpString(char* buf, size_t len);
    void     staIpString(char* buf, size_t len);
    bool     apClients();                      // к точке доступа подключены клиенты - поиск роутера приостановлен
    uint32_t offlineS();                       // сколько секунд нет связи с роутером (0 - есть или SSID не задан)
    bool     timeValid();                       // часы установлены (NTP или вручную)
    TimeSource timeSource();
    bool     setTimeManual(time_t utc);         // ручная установка (из веб-задачи); false - время вне диапазона.
                                                // Работающий NTP при следующей синхронизации вернёт своё время.
    // Время событий и истории хранится как аптайм (монотонные секунды от включения, 64-битный таймер,
    // без переполнения). Реальное время считается назад от текущих часов:
    // время = сейчас − (аптайм сейчас − аптайм события), поэтому корректировка часов (NTP или вручную)
    // сразу видна во всей истории.
    uint32_t uptimeS();                         // секунды от включения
    time_t   epochFromUptime(uint32_t eventUptimeS);   // реальное время (UTC) события; 0 - время неизвестно
    time_t   bootEpoch();                       // момент включения по текущим часам; 0 - время неизвестно
    void     ipString(char* buf, size_t len);   // IP станции или точки доступа
    int8_t   rssi();
}
