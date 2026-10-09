// ui.h - экран (CO2 крупными цифрами) и настройка через тачскрин / кнопки
#pragma once
#include <stdint.h>

namespace Ui {
    void begin();
    void setRadioReport(bool ok, const char* report);   // результат сверки регистров nRF24 (экран запуска)
    void setRadioRegsOk(bool ok);                       // то же после смены режима связи (без вывода)
    void update(uint32_t now);                          // вызывать из loop()
}
