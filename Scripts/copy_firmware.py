# PlatformIO extra_script (post): после сборки копирует прошивку в Build/<env>/
#   firmware_<плата>_<датчик>_<версия>.bin (для OTA и USB), bootloader.bin, partitions.bin + flash_cmd.txt.
#   Плата и датчик - из самого образа: строка FW_ID_MARKER FW_ID («CO2HUB_FWID=es3c28p_k22», config.h,
#   выводится в Serial при старте) - совпадает с тем, что собрано, как бы ни был выбран датчик
#   (config.h или -DCO2_SENSOR); версия - FW_VERSION из config.h.
#   Прежние firmware*.bin в папке удаляются - там всегда одна, последняя прошивка.
# Подключён в platformio.ini:  extra_scripts = post:Scripts/copy_firmware.py
import glob
import os
import re
import shutil

Import("env")  # noqa: F821  (объект SCons от PlatformIO)

# Адреса записи для ESP32-S3 (Arduino): bootloader 0x0, partitions 0x8000, app 0x10000
FLASH_LAYOUT = (("bootloader.bin", 0x0000), ("partitions.bin", 0x8000), ("firmware.bin", 0x10000))
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


def copy_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    board = env.subst("$PIOENV")
    out_dir = os.path.join(project_dir, "Build", board)
    os.makedirs(out_dir, exist_ok=True)
    for old in glob.glob(os.path.join(out_dir, "firmware*.bin")):
        os.remove(old)
    app_src = os.path.join(build_dir, "firmware.bin")
    ident = fw_id(app_src, board)
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
    with open(os.path.join(out_dir, "flash_cmd.txt"), "w", encoding="utf-8") as f:
        f.write("esptool.py --chip esp32s3 write_flash " + " ".join(lines) + "\n")
    print("Прошивка скопирована в", out_dir, "->", app_name)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)  # noqa: F821
