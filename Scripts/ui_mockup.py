# Макет экранов устройства (приблизительно, по разметке ui.cpp / Readme.md §8): главный экран ES3C28P
# (320x240) и T-Display-S3 (320x170) с датчиком температуры, карточка группы меню.
# Шрифты - системные моноширинные (на устройстве - U8g2 9x15 / 10x20, цифры CO2 - 7-сегментные Font7 x2).
#   python Scripts/ui_mockup.py  ->  work/png/ui_mockup.png
# Параметры главного экрана (по умолчанию - как в прошивке):
#   --temp-scale N   множитель шрифта температуры / влажности 10x20 (по умолч. TEMP_TEXT_SCALE из ui.cpp);
#                    не помещается рядом с иконками и поправкой - как на устройстве: x1, затем 9x15
#   --speed N        общая скорость = число иконок вентиляторов внизу (0..6, по умолч. 3)
#   --ovr N          общая поправка (0 - не показывается, по умолч. 1)
#   --co2 N, --temp T, --hum H   значения на экране (--hum "" - без влажности, как у DS18B20)
#   пример: python Scripts/ui_mockup.py --temp-scale 3 --speed 6 --ovr -2
import argparse
import math
import os
import re
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "work", "png", "ui_mockup.png")
SCALE = 2


def fw_const(name, default):
    """Числовая константа из ui.cpp (constexpr ... NAME = 2; / = 0.8f;) - макет по тем же значениям, что прошивка."""
    try:
        src = open(os.path.join(ROOT, "CO2_Sensor_NRF24", "src", "ui.cpp"), encoding="utf-8").read()
        v = re.search(r"\b" + name + r"\s*=\s*(-?\d+(?:\.\d+)?)", src).group(1)
        return float(v) if "." in v else int(v)
    except (OSError, AttributeError):
        return default


ap = argparse.ArgumentParser(description="Макет экранов устройства")
ap.add_argument("--temp-scale", type=int, default=fw_const("TEMP_TEXT_SCALE", 2))
ap.add_argument("--speed", type=int, default=3)
ap.add_argument("--ovr", type=int, default=1)
ap.add_argument("--co2", default="812")
ap.add_argument("--temp", default="23.4")
ap.add_argument("--hum", default="45%")
ap.add_argument("--digits-ttf", default="", help="TTF для больших цифр CO2 (напр. C:/Windows/Fonts/STENCIL.TTF); пусто - 7-сегментные")
ap.add_argument("--out", default="", help="файл картинки (по умолч. work/png/ui_mockup.png)")
ARGS = ap.parse_args()
if ARGS.out:
    OUT = ARGS.out if os.path.isabs(ARGS.out) else os.path.join(ROOT, ARGS.out)


def digits_font(path, height):
    """TTF, у которого цифра «8» высотой height px (подбор размера)."""
    if not path:
        return None
    size = height
    for _ in range(40):
        f = ImageFont.truetype(path, size)
        x0, y0, x1, y1 = f.getbbox("8")
        if abs((y1 - y0) - height) <= 1:
            break
        size = max(8, int(size * height / max(1, y1 - y0)))
    return f

WHITE, GREEN, YELLOW, CYAN, GREY, NAVY = (255, 255, 255), (0, 255, 0), (255, 255, 0), (0, 255, 255), (128, 128, 128), (0, 0, 128)
HEADER_H, INFO_LINE_H, MARGIN = 26, 22, 4
FAN_R, FAN_STEP = 8, 18                 # иконка вентилятора 16 px, шаг 18 (ui.cpp FAN_ICON_STEP)
DIGIT_W, DIGIT_H, SEG_T = 58, 96, 9      # 7-сегментные цифры (Font7 48 px x2)
DIGITS_FONT = digits_font(ARGS.digits_ttf, DIGIT_H)   # --digits-ttf: большие цифры TTF-шрифтом


def font(px):
    for name in ("consola.ttf", "cour.ttf", "DejaVuSansMono.ttf"):
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            pass
    return ImageFont.load_default()


F_TEXT, F_SMALL, F_BIG = font(19), font(15), font(38)

SEGS = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd", "6": "afgedc",
        "7": "abc", "8": "abcdefg", "9": "abcdfg"}


def seg_digit(d, x, y, ch, color):
    w, h, t = DIGIT_W, DIGIT_H, SEG_T
    m = h // 2
    box = {"a": (x + t, y, x + w - t, y + t), "g": (x + t, y + m - t // 2, x + w - t, y + m + t // 2),
           "d": (x + t, y + h - t, x + w - t, y + h), "f": (x, y + t, x + t, y + m - t // 2),
           "b": (x + w - t, y + t, x + w, y + m - t // 2), "e": (x, y + m + t // 2, x + t, y + h - t),
           "c": (x + w - t, y + m + t // 2, x + w, y + h - t)}
    for s in SEGS[ch]:
        d.rectangle(box[s], fill=color)


def fan_icon(d, cx, cy, color):
    for a in range(0, 360, 120):
        d.pieslice((cx - FAN_R, cy - FAN_R, cx + FAN_R, cy + FAN_R), a, a + 50, fill=color)
    d.ellipse((cx - 2, cy - 2, cx + 2, cy + 2), fill=color)


RED = (255, 0, 0)


def hum_size(k):
    """Влажность - своим шрифтом без масштаба (ui.cpp: humFont): к 10x20 x2 - Inconsolata 24 px, иначе 9x15."""
    return 24 if k == 2 else 15


def temp_width(t, h, char_w, k):
    """Ширина «23.4°  45%» по метрикам устройства: моноширинные 10x20 (10 px x k) / 9x15 (9 px);
    влажность - hum_size()."""
    w = len(t) * char_w + (2 + 4) * k                         # цифры + значок градуса (DEG_GAP + 2 * DEG_R)
    if h:
        w += int(font(hum_size(k)).getlength(h)) + 10 * k     # + влажность (TEMP_RH_GAP)
    return w


def temp_right(d, w, cy, t, h, color, left_x):
    """«23.4°  45%» у правого края (значок градуса - кружок, «C» не пишется). Температура 10x20 x --temp-scale,
    влажность мельче (hum_size); не помещается правее left_x - x1, затем 9x15 (как drawTemp() в ui.cpp)."""
    max_w = w - MARGIN - left_x - 10
    k = max(1, ARGS.temp_scale)
    cw, size = 10 * k, 19 * k
    if temp_width(t, h, cw, k) > max_w:
        k, cw, size = 1, 10, 19
        if temp_width(t, h, cw, k) > max_w:
            cw, size = 9, 15
    print("  температура: шрифт %d px на %d px (место %d px)" % (size, temp_width(t, h, cw, k), max_w))
    f, fh = font(size), font(hum_size(k))
    x = w - MARGIN
    if h:
        d.text((x, cy), h, font=fh, fill=color, anchor="rm")
        x -= d.textlength(h, font=fh) + 10 * k
    x -= 2 * k                                                  # центр значка градуса
    r = 2 * k
    d.ellipse((x - r, cy - 5 * k - r, x + r, cy - 5 * k + r), outline=color, width=k)
    d.text((x - 4 * k, cy), t, font=f, fill=color, anchor="rm")


BOTTOM_LINE_H, BTN_ROW_H = 44, 48


def main_screen(w, h, title, temp, hum, es_buttons=False, td_adjust=False):
    """Главный экран: шапка, цифры CO2 (между шапкой и рядом кнопок ES), нижняя строка (иконки, «+1»,
    температура x2) - видна всегда; ES - ряд кнопок над нижней строкой; TD - метки «+» / «-» режима скорости."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((MARGIN, HEADER_H // 2), "12:34  связь 3/3  Wi-Fi", font=F_TEXT, fill=GREEN, anchor="lm")
    touch = h > 200
    info_y = h - BOTTOM_LINE_H
    btn_y = info_y - (BTN_ROW_H if touch else 0)
    if es_buttons:                                   # строка статусов вентиляторов - только ES
        d.text((MARGIN, HEADER_H + 11), "1:3 2:4* 3:nRF?", font=F_SMALL, fill=WHITE, anchor="lm")
    co2 = ARGS.co2
    cy = (HEADER_H + 22 + btn_y) // 2 if touch else (HEADER_H + info_y) // 2
    if DIGITS_FONT:                                  # TTF: высота цифр - DIGIT_H, центр самих цифр - (w/2, cy)
        f = DIGITS_FONT
        bx0, by0, bx1, by1 = d.textbbox((0, 0), co2, font=f)
        if bx1 - bx0 > w - 2 * MARGIN:              # не влезает по ширине (широкий шрифт, 4 цифры) - уменьшить
            f = ImageFont.truetype(ARGS.digits_ttf, int(f.size * (w - 2 * MARGIN) / (bx1 - bx0)))
            bx0, by0, bx1, by1 = d.textbbox((0, 0), co2, font=f)
        d.text((w // 2 - (bx0 + bx1) // 2, cy - (by0 + by1) // 2), co2, font=f, fill=WHITE)
    else:                                            # как на устройстве: 7-сегментные Font7 x2
        total = len(co2) * DIGIT_W + (len(co2) - 1) * 12
        x0 = (w - total) // 2
        for i, ch in enumerate(co2):
            seg_digit(d, x0 + i * (DIGIT_W + 12), cy - DIGIT_H // 2, ch, WHITE)
    if es_buttons:                                   # ряд из 4 кнопок; «Выход» - сверху справа, как в меню
        bw = w // 4
        for i, t in enumerate(("+", "Меню", "[граф]", "-")):
            d.rounded_rectangle((i * bw + 2, btn_y + 2, i * bw + bw - 2, btn_y + BTN_ROW_H - 2), 6, fill=NAVY)
            d.text((i * bw + bw // 2, btn_y + BTN_ROW_H // 2), t, font=F_TEXT, fill=WHITE, anchor="mm")
        top = HEADER_H + MARGIN * 2
        d.rounded_rectangle((w - 90 + 2, 2, w - 2, top - 2), 6, fill=NAVY)
        d.text((w - 45, top // 2), "Выход", font=F_TEXT, fill=WHITE, anchor="mm")
    if td_adjust:
        for t, y in (("+", HEADER_H + 22 + 4), ("-", info_y - 28 - 4)):
            d.rounded_rectangle((2, y + 2, 20, y + 26), 6, fill=NAVY)
            d.text((11, y + 14), t, font=F_TEXT, fill=WHITE, anchor="mm")
    yb = info_y + BOTTOM_LINE_H // 2
    speed = max(0, min(6, ARGS.speed))
    for i in range(speed):
        fan_icon(d, MARGIN + FAN_R + i * FAN_STEP, yb, WHITE)
    text_x = MARGIN + speed * FAN_STEP + (MARGIN if speed else 0)
    ovr = "%+d" % ARGS.ovr if ARGS.ovr else ""
    d.text((text_x, yb), ovr, font=F_TEXT, fill=RED, anchor="lm")      # поправка - красным
    temp_right(d, w, yb, temp, hum, WHITE, text_x + len(ovr) * 10)   # поправка 10x20: 10 px на знак, как на устройстве
    return im, title


def menu_group(w, h):
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    top = HEADER_H + MARGIN * 2
    d.text((MARGIN, top // 2), "Настройки: группа 6/8", font=F_SMALL, fill=CYAN, anchor="lm")
    d.rounded_rectangle((w - 90 + 2, 2, w - 2, top - 2), 6, fill=NAVY)
    d.text((w - 45, top // 2), "Выход", font=F_TEXT, fill=WHITE, anchor="mm")
    bottom = h - 48
    vt = top + (bottom - top) // 3
    d.text((w // 2, (top + vt) // 2), "Параметров: 3", font=F_SMALL, fill=GREY, anchor="mm")
    d.text((w // 2, (vt + bottom) // 2), "История и датчики", font=font(26), fill=YELLOW, anchor="mm")
    for i, t in enumerate(("<", "-", "+", ">")):
        d.rounded_rectangle((i * 80 + 2, bottom + 2, i * 80 + 78, h - 2), 6, fill=NAVY)
        d.text((i * 80 + 40, bottom + 24), t, font=F_TEXT, fill=WHITE, anchor="mm")
    return im, "ES3C28P: меню - карточка группы"


def menu_item(w, h):
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    top = HEADER_H + MARGIN * 2
    d.text((MARGIN, top // 2), "История и датчики 2/3", font=F_SMALL, fill=CYAN, anchor="lm")
    d.rounded_rectangle((w - 90 + 2, 2, w - 2, top - 2), 6, fill=NAVY)
    d.text((w - 45, top // 2), "Назад", font=F_TEXT, fill=WHITE, anchor="mm")
    bottom = h - 48
    vt = top + (bottom - top) // 3
    d.text((w // 2, (top + vt) // 2), "Датчик температуры", font=F_TEXT, fill=WHITE, anchor="mm")
    d.text((w // 2, (vt + bottom) // 2), "SHT40", font=F_BIG, fill=YELLOW, anchor="mm")
    for i, t in enumerate(("<", "-", "+", ">")):
        d.rounded_rectangle((i * 80 + 2, bottom + 2, i * 80 + 78, h - 2), 6, fill=NAVY)
        d.text((i * 80 + 40, bottom + 24), t, font=F_TEXT, fill=WHITE, anchor="mm")
    return im, "ES3C28P: меню - пункт группы"


def history_chart(w, h):
    """График истории: CO2 - область (steelblue), температура - красная линия по своей шкале
    (мин..макс всего буфера, подписи справа вверху / внизу)."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((MARGIN, HEADER_H // 2), "История CO2  <-> 5 ч  x4", font=F_TEXT, fill=CYAN, anchor="lm")
    x0, y0, y1 = 44, HEADER_H + MARGIN, h - 48 - 18
    cols = w - MARGIN - x0
    lo, hi = 400, 1000
    for v in range(lo, hi + 1, 200):
        y = y1 - 1 - (v - lo) * (y1 - y0 - 1) // (hi - lo)
        d.line((x0, y, x0 + cols, y), fill=(32, 32, 32))
        d.text((x0 - 5, y), str(v), font=F_SMALL, fill=(200, 200, 200), anchor="rm")
    tlo, thi = 205, 241                                   # 0.1 °C
    prev = None
    for c in range(cols):
        x = x0 + cols - 1 - c
        co2 = int(700 + 180 * math.sin(c / 40.0) + 40 * math.sin(c / 9.0))
        y = y1 - 1 - (co2 - lo) * (y1 - y0 - 1) // (hi - lo)
        d.line((x, y, x, y1), fill=(70, 130, 180))
        if c < 200:                                       # старые записи - без температуры
            t = int(223 + 18 * math.sin(c / 55.0))
            yt = y1 - 1 - (t - tlo) * (y1 - y0 - 1) // (thi - tlo)
            if prev:
                d.line((prev, (x, yt)), fill=(255, 0, 0), width=2)
            prev = (x, yt)
    d.text((x0 + cols - 2, y0 + 1), "24.1", font=F_SMALL, fill=(255, 0, 0), anchor="ra")
    d.text((x0 + cols - 2, y1 - 2), "20.5", font=F_SMALL, fill=(255, 0, 0), anchor="rd")
    d.line((x0 - 1, y0, x0 - 1, y1), fill=(200, 200, 200))
    d.line((x0 - 1, y1, x0 + cols, y1), fill=(200, 200, 200))
    for i, xx in enumerate((60, 140, 220, 300)):
        d.text((xx, y1 + 4), ("09:00", "10:00", "11:00", "12:00")[i], font=F_SMALL, fill=(200, 200, 200), anchor="ma")
    sb = (w - 90) // 4
    for i, (t, bw) in enumerate((("<", sb), ("-", sb), ("Выход", 90), ("+", sb), (">", w - 3 * sb - 90))):
        bx = sum((sb, sb, 90, sb)[:i])
        d.rounded_rectangle((bx + 2, h - 46, bx + bw - 2, h - 2), 6, fill=NAVY)
        d.text((bx + bw // 2, h - 24), t, font=F_TEXT, fill=WHITE, anchor="mm")
    return im, "ES3C28P: график истории (CO2 + температура красным)"


def sys_card_td(lines, title):
    """Карточка «Состояние системы» на T-Display (320x170): шапка, 6 строк 9x15 по 19 px, подсказка внизу.
    Значок градуса - кружок (в шрифте его нет), в строках отмечен «`»."""
    w, h = 320, 170
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    top = HEADER_H + MARGIN * 2
    d.text((MARGIN, top // 2), "Система 1/4", font=F_SMALL, fill=CYAN, anchor="lm")
    bottom = h - 20
    y = top + (bottom - top - 6 * 19) // 2
    for i, s in enumerate(lines):
        col = WHITE if i == 0 else YELLOW
        x, cy = MARGIN, y + i * 19 + 9
        for k, part in enumerate(s.split("`")):
            if k:
                d.ellipse((x + 2, cy - 7 - 2, x + 6, cy - 7 + 2), outline=col)
                x += 8
            d.text((x, cy), part, font=F_SMALL, fill=col, anchor="lm")
            x += d.textlength(part, font=F_SMALL)
    d.line((0, y + 6 * 19, w, y + 6 * 19), fill=(60, 60, 60))
    d.text((MARGIN, bottom + 10), "Кн.1:след/назад Кн.2:+/-(долго)", font=F_SMALL, fill=GREY, anchor="lm")
    return im, title


# Задачи FreeRTOS для макета (типичный набор Arduino-ESP32 3.x + Wi-Fi + веб):
# имя, состояние (W/R/B/S/D), приоритет, ядро (-1 - любое), стек мин., доля 0,1 %; порядок - как SysInfo (по приоритету)
MOCK_TASKS = [("ipc0", "B", 24, 0, 528, 0), ("ipc1", "B", 24, 1, 548, 0), ("wifi", "B", 23, 0, 3604, 25),
              ("esp_timer", "B", 22, 0, 2980, 4), ("sys_evt", "B", 20, -1, 1356, 1), ("arduino_events", "B", 19, -1, 2412, 2),
              ("tiT", "B", 18, -1, 380, 12), ("httpd", "B", 5, -1, 4310, 3), ("Tmr Svc", "B", 1, 0, 1520, 0),
              ("loopTask", "W", 1, 1, 5232, 60), ("mdns", "B", 1, -1, 2104, 1), ("IDLE0", "R", 0, 0, 724, 470),
              ("IDLE1", "W", 0, 1, 732, 420)]
MOCK_CORE_LOAD = (3, 2)                                  # загрузка ядер, %
TASK_LINE_H, TASK_NAME_COLS, CHAR_W = 15, 12, 9          # ui.cpp: TASK_LINE_H, TASK_NAME_COLS; 9x15 - 9 px на знак
TASK_COL_STATE, TASK_COL_PRIO_E, TASK_COL_STACK_E, TASK_COL_SHARE_E = 13, 17, 23, 29
TASK_STACK_WARN_B = 512
RED = (255, 0, 0)


def task_list():
    """Строки как ui.cpp buildTaskList(): группы ядро 0, ядро 1, любое; заголовок группы - («grp», ядро)."""
    rows = []
    for core in (0, 1, -1):
        grp = [t for t in MOCK_TASKS if t[3] == core]
        if grp:
            rows.append(("grp", core))
            rows += grp
    return rows


def tasks_card(w, h, title, page):
    """Карточка «Задачи FreeRTOS» (ui.cpp drawTasks): таблица от menuTop строками 9x15 по TASK_LINE_H."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    touch = h == 240
    top = HEADER_H + MARGIN * 2
    bottom = h - (48 if touch else 20)
    per = (bottom - top) // TASK_LINE_H - 1
    rows = task_list()
    pages = (len(rows) + per - 1) // per
    d.text((MARGIN, top // 2), "Система 2/5", font=F_SMALL, fill=CYAN, anchor="lm")

    def cell(s, col, right, cy, color):
        d.text((MARGIN + col * CHAR_W, cy), s, font=F_SMALL, fill=color, anchor="rm" if right else "lm")

    cy = top + TASK_LINE_H // 2
    for s, col, right in (("Задача", 0, False), ("?", TASK_COL_STATE, False), ("Пр", TASK_COL_PRIO_E, True),
                          ("Стек", TASK_COL_STACK_E, True), ("Итог", TASK_COL_SHARE_E, True)):
        cell(s, col, right, cy, GREY)
    d.text((w - MARGIN, cy), "%d/%d" % (page + 1, pages), font=F_SMALL, fill=CYAN, anchor="rm")
    for r, t in enumerate(rows[page * per:(page + 1) * per]):
        cy = top + (r + 1) * TASK_LINE_H + TASK_LINE_H // 2
        if t[0] == "grp":
            cell("Любое ядро" if t[1] < 0 else "Ядро %d: загрузка %d%%" % (t[1], MOCK_CORE_LOAD[t[1]]), 0, False, cy, CYAN)
            continue
        name, st, prio, _core, stack, share = t
        cell(name[:TASK_NAME_COLS], 0, False, cy, YELLOW)
        cell(st, TASK_COL_STATE, False, cy, YELLOW)
        cell(str(prio), TASK_COL_PRIO_E, True, cy, YELLOW)
        cell(str(stack), TASK_COL_STACK_E, True, cy, RED if stack < TASK_STACK_WARN_B else YELLOW)
        cell("%d%%" % (share // 10) if share >= 10 else "<1%", TASK_COL_SHARE_E, True, cy, YELLOW)
    if touch:
        bw = w // 4
        for i, s in enumerate(("<", "-", "+", ">")):
            d.rounded_rectangle((i * bw + 2, bottom + 2, (i + 1) * bw - 3, h - 3), 6, fill=NAVY)
            d.text(((i + 0.5) * bw, bottom + 24), s, font=F_TEXT, fill=WHITE, anchor="mm")
    else:
        d.text((MARGIN, bottom + 10), "Кн.1:стр.+ Кн.2:стр.- ДН Кн.2:назад", font=F_SMALL, fill=GREEN, anchor="lm")
    return im, title


def main():
    # порядок на листе (2 в ряд): главные экраны, график истории, меню, карточки «Система», задачи FreeRTOS
    shots = [main_screen(320, 240, "ES3C28P: главный экран", ARGS.temp, ARGS.hum),
             main_screen(320, 170, "T-Display-S3: главный экран", ARGS.temp, ARGS.hum),
             main_screen(320, 240, "ES3C28P: после касания - кнопки", ARGS.temp, ARGS.hum, es_buttons=True),
             main_screen(320, 170, "T-Display-S3: режим скорости (КН Кн.2)", ARGS.temp, ARGS.hum, td_adjust=True),
             main_screen(320, 170, "T-Display-S3, DS18B20 (без влажности)", ARGS.temp, ""),
             history_chart(320, 240),
             menu_group(320, 240), menu_item(320, 240),
             sys_card_td(["Состояние системы", "CPU: 3%, 2%, 45.3`C", "Свободно RAM 180 Кб, PSRAM 7.6 Мб",
                          "Буфер: 7.0 сут (30240), 236 Кб", "Занято: 4.6 сут (19872)", "CO2 116 Кб, t` 39 Кб"],
                         "T-Display: Система - обычный случай"),
             sys_card_td(["Состояние системы", "CPU: 100%, 100%, 105.3`C", "Свободно RAM 210 Кб, PSRAM 7.9 Мб",
                          "Буфер: 365.0 сут (1017290), 8.1 Мб", "Занято: 365.0 сут (1017290)", "CO2 6.1 Мб, t` 2.0 Мб"],
                         "T-Display: Система - худший случай по длине"),
             tasks_card(320, 240, "ES3C28P: Система -> Задачи FreeRTOS («-» / «+» - страницы)", 0),
             tasks_card(320, 170, "T-Display: Задачи FreeRTOS (ДН Кн.1 - войти, КН - страницы)", 0),
             tasks_card(320, 240, "ES3C28P: Задачи, стр. 2", 1),
             tasks_card(320, 170, "T-Display: Задачи, стр. 2", 1)]
    pad, cap = 14, 22
    cols = 2
    cw, ch = 320 * SCALE, 240 * SCALE
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cw + pad) + pad, rows * (ch + cap + pad) + pad), (235, 238, 242))
    ds = ImageDraw.Draw(sheet)
    for i, (im, title) in enumerate(shots):
        x = pad + (i % cols) * (cw + pad)
        y = pad + (i // cols) * (ch + cap + pad)
        ds.text((x, y), title, font=font(18), fill=(30, 30, 30))
        big = im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST)
        sheet.paste(big, (x, y + cap))
        ds.rectangle((x - 1, y + cap - 1, x + big.width, y + cap + big.height), outline=(90, 90, 90))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT)
    print(OUT)


if __name__ == "__main__":
    main()
