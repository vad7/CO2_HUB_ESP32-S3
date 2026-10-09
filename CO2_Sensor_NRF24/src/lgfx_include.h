// lgfx_include.h - подключение LovyanGFX и шрифтов U8g2 без предупреждений -Wstrict-aliasing.
// Флаг -Wstrict-aliasing=1 включён для кода проекта (platformio.ini: src_build_flags), а заголовки
// библиотек читают PROGMEM через приведение указателей. Подключать библиотеки только отсюда.
#pragma once
#define LGFX_USE_V1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#include <LovyanGFX.hpp>
#include <U8g2lib.h>   // только шрифты u8g2_font_*_t_cyrillic
#pragma GCC diagnostic pop
