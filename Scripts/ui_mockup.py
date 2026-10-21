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
DIGIT_H_ES_SMOOTH = 110                  # сглаженные цифры ES3C28P (font_digits_es.h), T-Display - DIGIT_H
TOP_INSET_ES = 4                         # ES: шапка и «Выход» ниже края (ui.cpp TOP_INSET)
DIGITS_FONT = digits_font(ARGS.digits_ttf, DIGIT_H)   # --digits-ttf: большие цифры TTF-шрифтом
# сглаженный шрифт цифр прошивки (по умолчанию, include/font_digits.h - из этого TTF)
SMOOTH_TTF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "fonts",
                          "Montserrat-SemiBold.ttf")


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


def fan_icon(d, cx, cy, color, r=None):
    r = r or FAN_R
    for a in range(0, 360, 120):
        d.pieslice((cx - r, cy - r, cx + r, cy + r), a, a + 50, fill=color)
    hub = max(2, r // 4)
    d.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=color)


RED = (255, 0, 0)


def wifi_icon(d, cx, by, color):
    """Значок Wi-Fi (ui.cpp drawWifiIcon): точка r=1 (cx, by) и 3 дуги над ней, радиусы 5, 9, 13 px, толщина 2."""
    for r in (5, 9, 13):
        d.arc((cx - r, by - r, cx + r, by + r), 225, 315, fill=color, width=2)
    d.ellipse((cx - 1, by - 1, cx + 1, by + 1), fill=color)


def fan_off_icon(d, cx, cy, color, r=None):
    """Вентилятор, перечёркнутый красным крестиком (режим «вентиляторы выключены», ui.cpp drawFanOffIcon)."""
    r = r or FAN_R
    fan_icon(d, cx, cy, color, r)
    d.line((cx - r, cy - r, cx + r, cy + r), fill=RED, width=2)
    d.line((cx - r, cy + r, cx + r, cy - r), fill=RED, width=2)


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


def main_screen(w, h, title, temp, hum, es_buttons=False, td_adjust=False, ttf=None):
    """Главный экран: шапка, цифры CO2 (между шапкой и рядом кнопок ES), нижняя строка (иконки, «+1»,
    температура x2) - видна всегда; ES - ряд кнопок над нижней строкой; TD - метки «+» / «-» режима скорости."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    touch = h > 200
    top_in = TOP_INSET_ES if touch else 0
    hy = top_in + HEADER_H // 2
    d.text((MARGIN, hy), "12:34", font=font(hum_size(2)), fill=GREEN, anchor="lm")   # часы - шрифтом влажности; всё в порядке - без неполадок
    if not es_buttons:                               # при панели ES значок под «Выход» не рисуется
        wifi_icon(d, w - MARGIN - 13, hy + 5, (123, 125, 123))   # подключено - тёмно-серый (ui.cpp COL_DIM)
    info_y = h - BOTTOM_LINE_H
    btn_y = info_y + 8 - (BTN_ROW_H if touch else 0)  # ES: ряд кнопок низом до верха цифр нижней строки (BOTTOM_DIGITS_DY)
    if es_buttons:                                   # строка статусов вентиляторов - только ES
        d.text((MARGIN, top_in + HEADER_H + 11), "1:3 2:4* 3:nRF?", font=F_SMALL, fill=WHITE, anchor="lm")
    co2 = ARGS.co2
    cy = (top_in + HEADER_H + info_y) // 2           # по центру между шапкой и нижней строкой (панель - поверх)
    path = ttf or ARGS.digits_ttf                    # ttf - шрифт этой картинки, иначе --digits-ttf
    dfont = digits_font(ttf, DIGIT_H_ES_SMOOTH if touch else DIGIT_H) if ttf else DIGITS_FONT
    if dfont:                                        # TTF: высота цифр - DIGIT_H, центр самих цифр - (w/2, cy)
        f = dfont
        bx0, by0, bx1, by1 = d.textbbox((0, 0), co2, font=f)
        if bx1 - bx0 > w - 2 * MARGIN:              # не влезает по ширине (широкий шрифт, 4 цифры) - уменьшить
            f = ImageFont.truetype(path, int(f.size * (w - 2 * MARGIN) / (bx1 - bx0)))
            bx0, by0, bx1, by1 = d.textbbox((0, 0), co2, font=f)
        d.text((w // 2 - (bx0 + bx1) // 2, cy - (by0 + by1) // 2), co2, font=f, fill=WHITE)
    else:                                            # как на устройстве: 7-сегментные Font7 x2
        total = len(co2) * DIGIT_W + (len(co2) - 1) * 12
        x0 = (w - total) // 2
        for i, ch in enumerate(co2):
            seg_digit(d, x0 + i * (DIGIT_W + 12), cy - DIGIT_H // 2, ch, WHITE)
    if es_buttons:                                   # ряд из 5 кнопок; «Выход» - сверху справа, как в меню
        bw = w // 5
        for i, t in enumerate(("+", "Меню", "[граф]", "", "-")):
            d.rounded_rectangle((i * bw + 2, btn_y + 2, i * bw + bw - 2, btn_y + BTN_ROW_H - 2), 6, fill=NAVY)
            d.text((i * bw + bw // 2, btn_y + BTN_ROW_H // 2), t, font=F_TEXT, fill=WHITE, anchor="mm")
        fan_off_icon(d, 3 * bw + bw // 2, btn_y + BTN_ROW_H // 2, WHITE, 13)   # «вентиляторы выкл.»
        ex_b = cy - DIGIT_H_ES_SMOOTH // 2                # «Выход» высотой с нижние - низом до верха больших цифр (mainExitY)
        d.rounded_rectangle((w - 90 + 2, ex_b - BTN_ROW_H + 2, w - 2, ex_b - 2), 6, fill=NAVY)
        d.text((w - 45, ex_b - BTN_ROW_H // 2), "Выход", font=F_TEXT, fill=WHITE, anchor="mm")
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


HINT_H = 20                                  # T-Display: строка подсказки (ui.cpp HINT_H)
# подсказки T-Display - как ui.cpp (drawMenuStatic, drawHistoryStatic)
HINT_GROUPS = "Кн.2:пред Кн.1:след ДН:выход/вход"
HINT_ITEMS = "Кн.2:пред Кн.1:след ДН:назад/изм."
HINT_HISTORY = "Кн.2:<- Кн.1:-> ДН:выход/масштаб"


def board_name(h):
    return "ES3C28P" if h > 200 else "T-Display"


def menu_frame(d, w, h, head, btn, hint):
    """Рамка карточки меню: заголовок; ES - кнопка btn в шапке справа и ряд «< - + >» внизу; TD - подсказка внизу.
    Возвращает (top, bottom) области карточки."""
    top_in = TOP_INSET_ES if h > 200 else 0
    top = top_in + BTN_ROW_H if h > 200 else HEADER_H + MARGIN * 2   # ES: шапка карточки - до низа «Назад» (ui.cpp menuHdrH)
    d.text((MARGIN, (top_in + top) // 2), head, font=F_SMALL, fill=CYAN, anchor="lm")
    if h > 200:
        d.rounded_rectangle((w - 90 + 2, top_in + 2, w - 2, top - 2), 6, fill=NAVY)
        d.text((w - 45, (top_in + top) // 2), btn, font=F_TEXT, fill=WHITE, anchor="mm")
        bottom = h - 48
        for i, t in enumerate(("<", "-", "+", ">")):
            d.rounded_rectangle((i * 80 + 2, bottom + 2, i * 80 + 78, h - 2), 6, fill=NAVY)
            d.text((i * 80 + 40, bottom + 24), t, font=F_TEXT, fill=WHITE, anchor="mm")
    else:
        bottom = h - HINT_H
        d.text((MARGIN, bottom + HINT_H // 2), hint, font=F_SMALL, fill=GREY, anchor="lm")
    return top, bottom


def menu_group(w, h):
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bottom = menu_frame(d, w, h, "Настройки: группа 6/8", "Выход", HINT_GROUPS)
    vt = top + (bottom - top) // 3
    d.text((w // 2, (top + vt) // 2), "Параметров: 4", font=F_SMALL, fill=GREY, anchor="mm")
    d.text((w // 2, (vt + bottom) // 2), "История и датчики", font=font(26), fill=YELLOW, anchor="mm")
    return im, board_name(h) + ": меню - карточка группы"


def menu_item(w, h):
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bottom = menu_frame(d, w, h, "История и датчики 2/4", "Назад", HINT_ITEMS)
    vt = top + (bottom - top) // 3
    d.text((w // 2, (top + vt) // 2), "Датчик температуры", font=F_TEXT, fill=WHITE, anchor="mm")
    d.text((w // 2, (vt + bottom) // 2), "SHT40", font=F_BIG, fill=YELLOW, anchor="mm")
    return im, board_name(h) + ": меню - пункт группы"


def history_chart(w, h):
    """График истории: CO2 - область (steelblue), температура - красная линия по своей шкале
    (мин..макс всего буфера, подписи справа вверху / внизу)."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((MARGIN, HEADER_H // 2), "История CO2  <-> 5 ч  x4", font=F_TEXT, fill=CYAN, anchor="lm")
    touch = h > 200
    x0, y0, y1 = 44, HEADER_H + MARGIN, h - (48 if touch else HINT_H) - 18
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
    if touch:
        sb = (w - 90) // 4
        for i, (t, bw) in enumerate((("<", sb), ("-", sb), ("Выход", 90), ("+", sb), (">", w - 3 * sb - 90))):
            bx = sum((sb, sb, 90, sb)[:i])
            d.rounded_rectangle((bx + 2, h - 46, bx + bw - 2, h - 2), 6, fill=NAVY)
            d.text((bx + bw // 2, h - 24), t, font=F_TEXT, fill=WHITE, anchor="mm")
    else:
        d.text((MARGIN, h - HINT_H // 2), HINT_HISTORY, font=F_SMALL, fill=GREY, anchor="lm")
    return im, board_name(h) + ": график истории (CO2 + температура красным)"


def sys_card(w, h, lines, title):
    """Карточка «Состояние системы»: шапка, 6 строк 9x15 по 19 px; ES - кнопки меню, TD - подсказка внизу.
    Значок градуса - кружок (в шрифте его нет), в строках отмечен «`»."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bottom = menu_frame(d, w, h, "Система 1/8", "Назад", HINT_ITEMS)
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
    return im, board_name(h) + ": " + title


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
    top = TOP_INSET_ES + BTN_ROW_H if touch else HEADER_H + MARGIN * 2
    bottom = h - (48 if touch else 20)
    per = (bottom - top) // TASK_LINE_H - 1
    rows = task_list()
    pages = (len(rows) + per - 1) // per
    d.text((MARGIN, (TOP_INSET_ES if touch else 0) + (top - (TOP_INSET_ES if touch else 0)) // 2), "Система 2/8",
           font=F_SMALL, fill=CYAN, anchor="lm")

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
        d.text((MARGIN, bottom + 10), "Кн.2:стр.- Кн.1:стр.+ ДН Кн.2:назад", font=F_SMALL, fill=GREEN, anchor="lm")
    return im, title


# Журнал ошибок - пример (как ErrLog::timeRange + describe): время первого / последнего раза, повторы, что случилось
MOCK_ERRLOG = [
    ("20.10 09:15:02", "10:23:31", 210, "CO2 S8: нет ответа"),
    ("20.10 08:02:40", None, 1, "DS18B20: нет ответа датчика"),
    ("19.10 23:58:03", "20.10 07:12:45", 120, "Вентилятор 3 (Ванная): нет подтверждения (ACK)"),
    ("20.10 07:41:17", "07:41:59", 3, "CO2 S8: ошибка датчика 0x20: вне диапазона"),
    ("+0:00:01", None, 1, "nRF24: CONFIG = 0x00"),
    ("+0:00:01", None, 1, "nRF24: модуль не найден (нет ответа по SPI)"),
]


def errlog_card(w, h, title, page, shift=0, view=True, total=None):
    """Карточка «Журнал ошибок» (ui.cpp drawErrLog): запись - 2 строки 9x15: время (жёлтым), что случилось (белым).
    Длинная строка сдвигается влево на shift px, но не дальше своего конца (ES - свайпом, TD - бегущей строкой).
    ES: view=False - пункт меню: ряд «<» «Просмотр» «>», шапка - название и под ним «Записей: N»; view=True - просмотр:
    только «Назад», вместо названия «Записей: N  стр/стр», список до низа экрана. TD: «Записей: N  стр/стр» справа.
    total - сколько записей показать в шапке (по умолчанию - MOCK_ERRLOG)."""
    im = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(im)
    touch = h == 240
    top_in = TOP_INSET_ES if touch else 0
    top = top_in + BTN_ROW_H if touch else HEADER_H + MARGIN * 2
    bottom = (h if view else h - BTN_ROW_H) if touch else h - 20
    rows = (bottom - top) // TASK_LINE_H
    per = max(1, rows // 2)
    n_all = total or len(MOCK_ERRLOG)
    pages = (n_all + per - 1) // per
    paged = view or not touch
    cnt, pg = "Записей: %d" % n_all + ("  " if paged else ""), "%d/%d" % (page + 1, pages) if paged else ""
    if touch:
        d.rounded_rectangle((w - 90 + 2, top_in + 2, w - 2, top - 2), 6, fill=NAVY)
        d.text((w - 45, (top_in + top) // 2), "Назад", font=F_TEXT, fill=WHITE, anchor="mm")
        nav_h = BTN_ROW_H // 2                              # ui.cpp LOG_HDR_LINE_H
        if view:
            cy, x = (top_in + top) // 2, MARGIN
        else:
            d.text((MARGIN, top_in + nav_h // 2), "Система 2/9", font=F_SMALL, fill=CYAN, anchor="lm")
            cy, x = top_in + nav_h + (BTN_ROW_H - nav_h) // 2, MARGIN
    else:
        cy = top // 2
        d.text((MARGIN, cy), "Система 2/9", font=F_SMALL, fill=CYAN, anchor="lm")
        x = w - MARGIN - d.textlength(pg, font=F_SMALL) - d.textlength(cnt, font=F_SMALL)
    if touch and not view:
        w4, y = w // 4, h - BTN_ROW_H
        for x0, bw, s_ in ((0, w4, "<"), (w4, 2 * w4, "Просмотр"), (3 * w4, w - 3 * w4, ">")):
            d.rounded_rectangle((x0 + 2, y + 2, x0 + bw - 2, h - 2), 6, fill=NAVY)
            d.text((x0 + bw // 2, y + BTN_ROW_H // 2), s_, font=F_TEXT, fill=WHITE, anchor="mm")
    d.text((x, cy), cnt, font=F_SMALL, fill=GREY, anchor="lm")
    d.text((x + d.textlength(cnt, font=F_SMALL), cy), pg, font=F_SMALL, fill=CYAN, anchor="lm")
    avail = w - 2 * MARGIN
    for r, (t1, t2, n, what) in enumerate(MOCK_ERRLOG[page * per:(page + 1) * per]):
        y = top + r * 2 * TASK_LINE_H
        tl = t1 + (" - %s x%d" % (t2, n) if n > 1 else "")
        for k, (text, col) in enumerate(((tl, YELLOW), (what, WHITE))):
            line = Image.new("RGB", (w, TASK_LINE_H), (0, 0, 0))
            ld = ImageDraw.Draw(line)
            sh = min(shift, max(0, int(ld.textlength(text, font=F_SMALL)) - avail))
            ld.text((MARGIN - sh, TASK_LINE_H // 2), text, font=F_SMALL, fill=col, anchor="lm")
            ld.rectangle((0, 0, MARGIN - 1, TASK_LINE_H), fill=(0, 0, 0))            # поля (clip)
            ld.rectangle((w - MARGIN, 0, w, TASK_LINE_H), fill=(0, 0, 0))
            im.paste(line, (0, y + k * TASK_LINE_H))
    if not touch:
        d.text((MARGIN, bottom + 10), "Кн.2:стр.- Кн.1:стр.+ ДН Кн.2:назад", font=F_SMALL, fill=GREEN, anchor="lm")
    return im, title


def main():
    # строки листа: слева - ES3C28P, справа - T-Display (None - картинки для этой платы нет, ячейка пустая).
    # Порядок: главные экраны (7-сегментный, сглаженный шрифт), график истории, меню, «Система», задачи FreeRTOS.
    t, hm = ARGS.temp, ARGS.hum
    smooth = SMOOTH_TTF if os.path.exists(SMOOTH_TTF) else None
    rows = [(main_screen(320, 240, "ES3C28P: главный экран, цифры 7-сегментные", t, hm),
             main_screen(320, 170, "T-Display-S3: главный экран, цифры 7-сегментные", t, hm))]
    if smooth:
        rows.append((main_screen(320, 240, "ES3C28P: главный экран, цифры сглаженные (Montserrat)", t, hm, ttf=smooth),
                     main_screen(320, 170, "T-Display-S3: главный экран, сглаженные (Montserrat)", t, hm, ttf=smooth)))
    sys_n = ["Состояние системы", "CPU: 3%, 2%, 45.3`C", "Свободно RAM 180 Кб, PSRAM 7.6 Мб",
           "Буфер: 7.0 сут (30240), 236 Кб", "Занято: 4.6 сут (19872)", "CO2 116 Кб, t` 39 Кб"]
    sys_w = ["Состояние системы", "CPU: 100%, 100%, 105.3`C", "Свободно RAM 210 Кб, PSRAM 7.9 Мб",
           "Буфер: 365.0 сут (1017290), 8.1 Мб", "Занято: 365.0 сут (1017290)", "CO2 6.1 Мб, t` 2.0 Мб"]
    rows += [(main_screen(320, 240, "ES3C28P: после касания - кнопки", t, hm, es_buttons=True),
              main_screen(320, 170, "T-Display-S3: режим скорости (КН Кн.2)", t, hm, td_adjust=True)),
             (main_screen(320, 240, "ES3C28P, DS18B20 (без влажности)", t, ""),
              main_screen(320, 170, "T-Display-S3, DS18B20 (без влажности)", t, "")),
             (history_chart(320, 240), history_chart(320, 170)),
             (menu_group(320, 240), menu_group(320, 170)),
             (menu_item(320, 240), menu_item(320, 170)),
             (sys_card(320, 240, sys_n, "Система - обычный случай"), sys_card(320, 170, sys_n, "Система - обычный случай")),
             (sys_card(320, 240, sys_w, "Система - худший случай по длине"),
              sys_card(320, 170, sys_w, "Система - худший случай по длине")),
             (errlog_card(320, 240, "ES3C28P: Система -> Журнал ошибок («Просмотр» - листать)", 0, view=False),
              errlog_card(320, 240, "ES3C28P: просмотр журнала (свайп вверх / вниз - страницы)", 0)),
             (errlog_card(320, 240, "ES3C28P: просмотр, свайп влево - длинные строки сдвинуты", 0, 90),
              errlog_card(320, 170, "T-Display: Журнал ошибок (КН - страницы, длинные - бегущей строкой)", 0)),
             (tasks_card(320, 240, "ES3C28P: Система -> Задачи FreeRTOS («-» / «+» - страницы)", 0),
              tasks_card(320, 170, "T-Display: Задачи FreeRTOS (ДН Кн.1 - войти, КН - страницы)", 0)),
             (tasks_card(320, 240, "ES3C28P: Задачи, стр. 2", 1),
              tasks_card(320, 170, "T-Display: Задачи, стр. 2", 1))]
    pad, cap = 14, 22
    cols = 2
    cw, ch = 320 * SCALE, 240 * SCALE
    sheet = Image.new("RGB", (cols * (cw + pad) + pad, len(rows) * (ch + cap + pad) + pad), (235, 238, 242))
    ds = ImageDraw.Draw(sheet)
    for r, row in enumerate(rows):
        for c, shot in enumerate(row):
            if shot is None:
                continue
            im, title = shot
            x = pad + c * (cw + pad)
            y = pad + r * (ch + cap + pad)
            ds.text((x, y), title, font=font(18), fill=(30, 30, 30))
            big = im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST)
            sheet.paste(big, (x, y + cap))
            ds.rectangle((x - 1, y + cap - 1, x + big.width, y + cap + big.height), outline=(90, 90, 90))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT)
    print(OUT)
    # отдельные картинки для начала Readme: главный экран ES (первая на листе) и график истории ES
    for name, shot in (("ui_main_es.png", rows[0][0]), ("ui_history_es.png", history_chart(320, 240))):
        p = os.path.join(os.path.dirname(OUT), name)
        shot[0].resize((shot[0].width * SCALE, shot[0].height * SCALE), Image.NEAREST).save(p)
        print(p)


if __name__ == "__main__":
    main()
