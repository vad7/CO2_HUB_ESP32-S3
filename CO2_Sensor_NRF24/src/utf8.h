// utf8.h — строки UTF-8 в полях фиксированного размера (имена вентиляторов, SSID и т.п.)
// Тест на ПК: sh Scripts/test_utf8.sh
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Длина ведущего байта последовательности UTF-8 (1 — ASCII или испорченный байт)
inline size_t utf8SeqLen(uint8_t lead)
{
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// Копирование с обрезкой по границе символа: в конце не остаётся половины буквы
// (кириллица — 2 байта, «№» — 3 байта). size — размер dst вместе с '\0'.
inline void copyUtf8(char* dst, const char* src, size_t size)
{
    if (size == 0) return;
    size_t len = 0;
    while (src[len] != '\0') {
        const size_t n = utf8SeqLen((uint8_t)src[len]);
        if (len + n > size - 1) break;            // символ не помещается целиком
        size_t k = 1;
        while (k < n && src[len + k] != '\0') k++;
        if (k < n) break;                         // оборванная последовательность в конце src
        len += n;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}
