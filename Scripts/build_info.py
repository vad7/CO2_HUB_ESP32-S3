# Дата и время сборки прошивки -> "CO2_Sensor_NRF24/include/build_info.h" (FW_BUILD_DATE).
# __DATE__ / __TIME__ не годятся: это дата компиляции конкретного .cpp, а он пересобирается
# только при изменении — показывалась бы устаревшая дата. Заголовок переписывается при каждой
# сборке, поэтому включающие его файлы (main.cpp, ui.cpp, web.cpp) пересобираются всегда.
# Запуск: автоматически перед сборкой (platformio.ini: extra_scripts = pre:Scripts/build_info.py)
#         или вручную: python Scripts/build_info.py
import os
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) if "__file__" in globals() else os.getcwd()


def generate(root):
    out = os.path.join(root, "CO2_Sensor_NRF24", "include", "build_info.h")
    stamp = time.strftime("%d.%m.%Y %H:%M")
    text = ("// Сгенерировано Scripts/build_info.py при сборке — не править вручную.\n"
            "#pragma once\n"
            "#define FW_BUILD_DATE \"%s\"   // дата и время сборки (местное время ПК)\n" % stamp)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print("build_info: FW_BUILD_DATE = %s" % stamp)


try:
    Import("env")  # noqa: F821 — запуск из PlatformIO
    generate(env.subst("$PROJECT_DIR"))  # noqa: F821
except NameError:
    if __name__ == "__main__":
        generate(ROOT)
