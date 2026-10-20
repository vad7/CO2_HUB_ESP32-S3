// ui.h - экран (CO2 крупными цифрами) и настройка через тачскрин / кнопки
#pragma once
#include <stdint.h>

namespace Ui {
    void begin();
    void showRadioStatus(bool found);                   // экран запуска: nRF24 найден / нет (подробности - журнал ошибок)
    void update(uint32_t now);                          // вызывать из loop()
}
