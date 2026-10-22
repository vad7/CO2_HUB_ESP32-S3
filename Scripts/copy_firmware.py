# PlatformIO extra_script (post): после сборки копирует прошивку в Build/<env>/
#   firmware_<плата>_<датчик>_<версия>.bin (для OTA и USB), bootloader.bin, partitions.bin + flash_cmd.txt;
#   firmware_<плата>_<датчик>_<версия>_full.bin - полный образ (firmware.factory.bin сборки: загрузчик, разделы,
#   boot_app0, программа) одним файлом с адреса 0x0 - для первой прошивки из браузера (Readme §11), не для OTA.
#   Плата и датчик - из самого образа: строка FW_ID_MARKER FW_ID («CO2HUB_FWID=es3c28p_k22», config.h,
#   выводится в Serial при старте) - совпадает с тем, что собрано, как бы ни был выбран датчик
#   (config.h или -DCO2_SENSOR); версия - FW_VERSION из config.h.
#   Прежние firmware_<плата>_<датчик>_*.bin этого же датчика удаляются - в папке по одной, последней прошивке
#   на датчик (K22 и S8 лежат рядом); команда записи - flash_cmd_<плата>_<датчик>.txt.
# Подключён в platformio.ini:  extra_scripts = post:Scripts/copy_firmware.py
import glob
import os
import re
import shutil

Import("env")  # noqa: F821  (объект SCons от PlatformIO)

# Адреса записи для ESP32-S3 (Arduino): bootloader 0x0, partitions 0x8000, app 0x10000
FLASH_LAYOUT = (("bootloader.bin", 0x0000), ("partitions.bin", 0x8000), ("firmware.bin", 0x10000))
FACTORY_BIN = "firmware.factory.bin"    # полный образ сборки (pioarduino: «combined binary image»), пишется с 0x0
FULL_SUFFIX = "_full"                   # имя полного образа: firmware_<id>_<версия>_full.bin (веб OTA его не примет)
CONFIG_H = os.path.join("CO2_Sensor_NRF24", "include", "config.h")
FW_ID_MARKER = b"CO2HUB_FWID="          # = FW_ID_MARKER в config.h
FW_ID_RE = re.compile(rb"CO2HUB_FWID=([a-z0-9_]+)")


def fw_version(project_dir):
    with open(os.path.join(project_dir, CONFIG_H), encoding="utf-8") as f:
        m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
    return m.group(1) if m else "unknown"


def fw_id(image_path, fallback):
    with open(image_path, "rb") as f:
        m = FW_ID_RE.search(f.read())
    return m.group(1).decode() if m else fallback


def flash_cmd_name(ident):
    return "flash_cmd_%s.txt" % ident                 # команда esptool для прошивки этого датчика


def copy_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    board = env.subst("$PIOENV")
    out_dir = os.path.join(project_dir, "Build", board)
    os.makedirs(out_dir, exist_ok=True)
    app_src = os.path.join(build_dir, "firmware.bin")
    ident = fw_id(app_src, board)
    for old in glob.glob(os.path.join(out_dir, "firmware_%s_*.bin" % ident)):   # прежние версии этого датчика
        os.remove(old)
    old_cmd = os.path.join(out_dir, "flash_cmd.txt")  # прежнее имя (одна прошивка на папку)
    if os.path.exists(old_cmd):
        os.remove(old_cmd)
    if ident == board:
        print("ВНИМАНИЕ: в образе нет метки %s - датчик в имени файла не указан" % FW_ID_MARKER.decode())
    app_name = "firmware_%s_%s.bin" % (ident, fw_version(project_dir))
    lines = []
    for name, addr in FLASH_LAYOUT:
        src = os.path.join(build_dir, name)
        if os.path.isfile(src):
            dst = app_name if name == "firmware.bin" else name
            shutil.copy2(src, os.path.join(out_dir, dst))
            lines.append("0x%05X %s" % (addr, dst))
    with open(os.path.join(out_dir, flash_cmd_name(ident)), "w", encoding="utf-8") as f:
        f.write("esptool.py --chip esp32s3 write_flash " + " ".join(lines) + "\n")
    print("Прошивка скопирована в", out_dir, "->", app_name)
    copy_full_image(source, target, env)


def copy_full_image(source, target, env):
    """Полный образ: платформа собирает его раньше copy_firmware (post-действие на тот же firmware.bin,
    добавлено до extra_scripts) - копируется вместе с прошивкой."""
    build_dir = env.subst("$BUILD_DIR")
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), "Build", env.subst("$PIOENV"))
    src = os.path.join(build_dir, FACTORY_BIN)
    if not os.path.isfile(src):
        print("ВНИМАНИЕ: нет %s - полный образ для прошивки из браузера не создан" % FACTORY_BIN)
        return
    ident = fw_id(os.path.join(build_dir, "firmware.bin"), env.subst("$PIOENV"))
    name = "firmware_%s_%s%s.bin" % (ident, fw_version(env.subst("$PROJECT_DIR")), FULL_SUFFIX)
    shutil.copy2(src, os.path.join(out_dir, name))
    with open(os.path.join(out_dir, flash_cmd_name(ident)), "a", encoding="utf-8") as f:
        f.write("# или одним файлом: esptool.py --chip esp32s3 write_flash 0x0 %s\n" % name)
    print("Полный образ (с адреса 0x0) ->", name)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)  # noqa: F821
