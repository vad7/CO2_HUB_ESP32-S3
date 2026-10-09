# Упаковка веб-файлов "CO2_Sensor_NRF24/web/" в заголовок "CO2_Sensor_NRF24/include/web_files.h".
#   *.htm        - страницы (меню и подвал вставляет web/app.js в #app_menu / #app_footer);
#   *.js, *.css  - скрипты и стили; ссылки на них в страницах получают ?h=<crc32> (см. add_versions).
# Всё хранится сжатым gzip и отдаётся с Content-Encoding: gzip - прошивка ничего не подставляет,
# данные страницы берут из JSON /api/vars (см. web/app.js).
# Всё встроено во Flash: страницы работают без доступа в интернет.
# Запуск: автоматически перед сборкой (platformio.ini: extra_scripts = pre:Scripts/build_web.py)
#         или вручную: python Scripts/build_web.py
# Функция render_page() используется и сервером предпросмотра Scripts/web_preview.py.
import gzip
import os
import re
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) if "__file__" in globals() else os.getcwd()
WEB_DIR = os.path.join(ROOT, "CO2_Sensor_NRF24", "web")
OUT = os.path.join(ROOT, "CO2_Sensor_NRF24", "include", "web_files.h")

MIME = {".htm": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8"}
ASSET_RE = re.compile(r'(src|href)="/([\w.-]+\.(?:js|css))"')


def render_page(web_dir, name):
    """Страница в том виде, в каком она попадает в прошивку."""
    with open(os.path.join(web_dir, name), "r", encoding="utf-8") as f:
        return add_versions(web_dir, f.read())


def add_versions(web_dir, html):
    """Ссылки на свои .js/.css получают ?h=<crc32 содержимого>: браузер кэширует их на сутки
    (CACHE_STATIC в web.cpp), а новая прошивка с изменённым файлом даёт новый адрес.
    Прошивка строку запроса у статических файлов отбрасывает."""
    def sub(m):
        name = m.group(2)
        full = os.path.join(web_dir, name)
        if not os.path.isfile(full):
            return m.group(0)
        with open(full, "rb") as f:
            h = zlib.crc32(f.read()) & 0xFFFFFFFF
        return '%s="/%s?h=%08x"' % (m.group(1), name, h)
    return ASSET_RE.sub(sub, html)


def c_array(data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join("0x%02X" % b for b in data[i:i + 24]) + ",")
    return "\n".join(lines)


def generate():
    files = sorted(f for f in os.listdir(WEB_DIR) if os.path.splitext(f)[1] in MIME)
    parts = ["// Сгенерировано Scripts/build_web.py из \"CO2_Sensor_NRF24/web/\" - не править вручную.",
             "#pragma once", "#include <stdint.h>", "#include <stddef.h>", "",
             "struct WebFile {", "    const char*    path;      // URL без ведущего '/'",
             "    const uint8_t* data;      // gzip", "    size_t         len;", "    const char*    mime;",
             "    bool           html;      // страница (не кэшировать: меняется с прошивкой)",
             "};", ""]
    table = []
    for i, name in enumerate(files):
        ext = os.path.splitext(name)[1]
        if ext == ".htm":
            raw = render_page(WEB_DIR, name).encode("utf-8")
        else:
            with open(os.path.join(WEB_DIR, name), "rb") as f:
                raw = f.read()
        data = gzip.compress(raw, 9, mtime=0)
        parts.append("// %s: %d байт, gzip %d" % (name, len(raw), len(data)))
        parts.append("static const uint8_t WEB_DATA_%d[] = {\n%s\n};" % (i, c_array(data)))
        table.append('    { "%s", WEB_DATA_%d, sizeof(WEB_DATA_%d), "%s", %s },'
                     % (name, i, i, MIME[ext], "true" if ext == ".htm" else "false"))
    parts.append("")
    parts.append("static const WebFile WEB_FILES[] = {\n%s\n};" % "\n".join(table))
    parts.append("constexpr size_t WEB_FILES_COUNT = sizeof(WEB_FILES) / sizeof(WEB_FILES[0]);")
    text = "\n".join(parts) + "\n"
    old = None
    if os.path.isfile(OUT):
        with open(OUT, "r", encoding="utf-8") as f:
            old = f.read()
    if old != text:   # перезапись только при изменении - без лишней пересборки
        with open(OUT, "w", encoding="utf-8") as f:
            f.write(text)
        print("build_web: web_files.h обновлён (%d файлов)" % len(files))


try:
    Import("env")  # noqa: F821 - запуск из PlatformIO
    ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
    WEB_DIR = os.path.join(ROOT, "CO2_Sensor_NRF24", "web")
    OUT = os.path.join(ROOT, "CO2_Sensor_NRF24", "include", "web_files.h")
    generate()
except NameError:
    if __name__ == "__main__":   # вручную; при импорте из web_preview.py - ничего не делать
        generate()
