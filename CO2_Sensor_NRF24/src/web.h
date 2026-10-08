// web.h — веб-интерфейс (перенос веб-интерфейса хаба ESP8266: index, settings (все настройки по группам), setfans, history)
#pragma once

namespace Web {
    void begin();                                // запуск HTTP-сервера (своя задача FreeRTOS); уже запущен — ничего
    void setRadioReport(const char* report);     // отчёт сверки регистров nRF24 для страницы «Система»
}
