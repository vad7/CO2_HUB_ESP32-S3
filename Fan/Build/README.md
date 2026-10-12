# Build

Содержимое собирается командой (из корня проекта):

```
python scripts/build.py
```

| Файл | Что это |
|---|---|
| `WirelessCO2Fan.hex` | прошивка flash, Intel HEX |
| `WirelessCO2Fan_eeprom.bin` | образ EEPROM, 256 байт, двоичный (генератор — `scripts/make_eeprom.py`) |
| `WirelessCO2Fan.elf` / `.lss` / `.map` | отладочные, листинг, карта памяти |

Ключи взяты не из `.cproj`, а из Makefile, который генерирует сама Atmel Studio
(образец — `OnAS6.2/Makefile`). Критично: `-ffunction-sections -fdata-sections` и
`-Wl,--gc-sections` в настройках проекта не показываются, но AS ставит их всегда.
Без них в прошивку попадают неиспользуемые функции (+152 байта).

Проверка: `Build/WirelessCO2Fan.hex` побайтно совпадает с `OnAS6.2/WirelessCO2Fan.hex`.

Фьюзы записаны в шапке `WirelessCO2Fan.c` после слов `// Fuses:`.
