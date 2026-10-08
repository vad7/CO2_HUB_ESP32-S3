# Корпус для датчика CO2: плата с экраном (ES3C28P или LILYGO T-Display-S3) + K22 (снизу) + nRF24L01+ (за экраном).
# Две крышки: «на стену» (щели над K22) и «на стол». Параметрический (CadQuery): все размеры — в блоках PARAMETERS.
#   python Scripts/enclosure.py                       — собрать корпуса для обеих плат
#   python Scripts/enclosure.py --board es3c28p       — только ES3C28P  (или --board tdisplay_s3 / tdisplay_s3_s8)
#   tdisplay_s3_s8 — T-Display-S3 + SenseAir S8 LP: компактный корпус (приоритет — на стену), плата — на ушах
#   (TDisplayS3_S8_ears_x4.stl, саморезы M3); S8 поднят к крышке на 2 рёбрах, прижат 2 штырьками крышки.
#   T-Display: штырьки кнопок BOOT / IO14 — <ПЛАТА>_buttons_x2.stl (печать наконечником вниз).
#   --update-project  — после сборки обновить таблицу параметров в Readme.md (подраздел «### 13.7 Параметры»)
# Результат в Build/enclosure/ (файлы названы по плате):
#   <ПЛАТА>_body.stl, <ПЛАТА>_lid_wall.stl (щели над K22, на стену), <ПЛАТА>_lid_table.stl (без щелей, на стол),
#   <ПЛАТА>_assembly.step, <ПЛАТА>_layout.png (чертёж), <ПЛАТА>_preview.png; ПЛАТА = ES3C28P | TDisplayS3.
#   TDisplayS3_clips_x4.stl — 4 защёлки платы (печать плашмя, вклеить в гнёзда крышки ацетоном/ABS-клеем).
# Всё рассчитано на печать БЕЗ ПОДДЕРЖЕК: щели вертикальные (нет мостов), заужения — каплевидные или открытые вверх,
# отверстия крышки — вдоль оси печати. Заужения под сверление сделаны с ВНУТРЕННЕЙ стороны стенок (снаружи гладко).
# Исходные данные: Docs/ES3C28P_Size.pdf, Docs/ES3C28P_3D.zip (STEP), Docs/T-Display-S3_full.stp (STEP LILYGO),
# Docs/K22 size.png (K22-PWM), nRF24L01+ 16 x 29 мм. Разбор STEP — Scripts/step_analyze.py.
#
# Система координат корпуса: X вправо, Y вверх, Z от стены к зрителю; Z = 0 — задняя стенка (к стене),
# Y = 0 — низ корпуса, X = 0 — середина. Вид спереди: сверху экран, снизу K22.
import json
import math
import os
import re
import subprocess
import sys

import cadquery as cq

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Build", "enclosure")
README_MD = os.path.join(ROOT, "Readme.md")

# варианты: плата + датчик (tdisplay_s3_s8 — T-Display-S3 + SenseAir S8 LP, компактный корпус, крепление ушами)
BOARDS = {"es3c28p": "ES3C28P", "tdisplay_s3": "TDisplayS3", "tdisplay_s3_s8": "TDisplayS3_S8"}


def _arg_board():
    for i, a in enumerate(sys.argv):
        if a.startswith("--board="):
            return a.split("=", 1)[1]
        if a == "--board" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return "es3c28p"


BOARD = _arg_board()
if BOARD not in BOARDS:
    raise SystemExit("неизвестная плата %r, допустимо: %s" % (BOARD, ", ".join(BOARDS)))
IS_TD = BOARD.startswith("tdisplay_s3")
IS_S8 = BOARD.endswith("_s8")          # датчик SenseAir S8 LP вместо K22 (только с T-Display)
PREFIX = BOARDS[BOARD]

# ===================================== PARAMETERS =====================================
# Крепёж и диаметры отверстий — в каждой группе строкой «# Крепёж: …» и параметрами *_PILOT / *_HOLE_D / *_DEPTH:
#   *_PILOT  — глухое отверстие под саморез (винт сам нарезает резьбу в пластике);
#   *_HOLE_D — сквозное отверстие (винт проходит свободно);  *_DEPTH — глубина глухого отверстия.
# Максимальная длина винта = толщина прижимаемой детали + глубина отверстия (длиннее — упрётся в дно).
# --- общие ---
WALL = 2.4            # боковые стенки
BACK_T = 2.4          # задняя стенка (дно)
LID_T = 2.2           # лицевая пластина крышки
R_OUT = 4.0           # радиус внешних вертикальных рёбер
R_IN = 1.6            # радиус внутренних углов
FIT = 0.3             # зазор посадки (подберите под свой принтер)
SHRINK_XY = 0.006     # компенсация усадки материала в плоскости стола: STL масштабируются на (1 + SHRINK_XY).
                      # ABS (Picaso Designer X): стойки 78 мм выходили на ~0.5 мм короче -> 0.6 %. PLA/PETG ~0.002, 0 — без.
                      # В слайсере масштаб НЕ менять (будет двойная компенсация).
SHRINK_Z = 0.0        # то же по высоте (по Z размеры некритичны)
GLASS_FIT = 0.3       # ES3C28P: зазор рамки вокруг стекла на сторону (усадка компенсируется SHRINK_XY)
GLASS_RING_T = 2.0    # ES3C28P: толщина рамки вокруг стекла
CORNER_RELIEF_D = 2.0 # ES3C28P: круглые вырезы во внутренних углах рамки — прямой угол стекла не упирается в скругление
# ES3C28P: со стороны USB из-под стекла выходят шлейфы экрана и тача — рамка там пониженная, полная высота
# только у углов (длина от нижнего / верхнего угла стекла)
GLASS_FLEX_FULL_BOT = 2.0   # полная высота рамки от нижнего угла стекла, мм
GLASS_FLEX_FULL_TOP = 8.0   # полная высота рамки от верхнего угла стекла, мм
GLASS_FLEX_H = 1.0          # высота пониженной части рамки (от лицевой пластины)
USB_SIDE = "left"     # сторона Type-C у обеих плат: "left" / "right"

# --- плата ES3C28P (чертёж ES3C28P_Size.pdf, STEP) ---
PCB_L, PCB_W, PCB_T = 86.0, 50.0, 1.6      # плата 86 x 50 x 1.6 (в корпусе длинная сторона — по X)
GLASS_L, GLASS_W = 69.2, 50.0              # стекло с тачем
GLASS_H = 4.3                              # стекло над платой (тач 1.0 + клей + LCD 2.3 + клей)
BACK_PARTS_H = 4.7                         # детали с обратной стороны платы
HOLE_DX, HOLE_DY = 78.0, 42.0              # крепёжные отверстия платы (по длине / по ширине)
PCB_HOLE_D = 3.2                           # отверстия платы ES3C28P (справочно, по чертежу)
VA_L, VA_W = 58.05, 43.6                   # видимая область экрана
VA_OFFSET_X = 2.835                        # смещение центра VA от центра платы (вдоль длины); окно центрируется с запасом на него
VA_MARGIN = 0.2                            # окно больше VA с каждой стороны
USB_W, USB_H = 13.0, 8.0                   # вырез под Type-C с корпусом штекера (обе платы)
USB_MIN_BRIDGE = 1.5                       # перемычка стенки над вырезом тоньше — вырез открывается до верха стенки
USB_BODY_H = 3.25                          # ES3C28P: корпус Type-C стоит на ТЫЛЬНОЙ стороне платы (не утоплен),
                                           # от тыла платы до -4.85 по STEP (габарит -0.5 — это выводы в плате)
USB_Z_FROM_PCB_FRONT = -(PCB_T + USB_BODY_H / 2)   # центр разъёма относительно лицевой стороны платы = -3.225
# Крепёж: плата ES3C28P -> 4 стойки на крышке, винт M3 × 6 (саморез по пластику), макс. длина 7 = 1.6 + 5.5
POST_D, POST_PILOT = 6.2, 2.6              # ES3C28P: стойки платы на крышке: диаметр стойки, отверстие под саморез M3
POST_PILOT_DEPTH = 5.5                     # глубина отверстия от лица платы (в стойку и лицевую пластину крышки)
POST_H = GLASS_H + 0.15                    # высота стоек = стекло + зазор
CLR_PCB = 2.0                              # ES3C28P: под платой экрана (рамка стекла 1.6 + зазор)
CLR_SIDE = 1.0                             # ES3C28P: зазор по бокам платы экрана

# --- плата LILYGO T-Display-S3 (STEP LILYGO: Docs/T-Display-S3_full.stp), без крепёжных отверстий ---
# Крепёж: винтов нет — 4 защёлки (TDisplayS3_clips_x4.stl) вклеиваются в гнёзда крышки
TD_PCB_L, TD_PCB_W, TD_PCB_T = 60.78, 25.5, 1.55   # плата (длина вдоль X корпуса, ширина, толщина)
TD_LCD_L, TD_LCD_W, TD_LCD_Y0 = 56.2, 25.95, 4.58  # модуль дисплея: длина, ширина, начало от края USB
TD_GLASS_L, TD_GLASS_W, TD_GLASS_Y0 = 49.87, 25.95, 6.66   # стекло: длина, ширина, начало от края USB
TD_STACK_H = 4.83                          # от переднего стекла до лицевой стороны платы
TD_BACK_H = 3.5                            # детали с обратной стороны платы
TD_USB_X, TD_USB_D = -0.13, 3.6            # Type-C: смещение от оси платы; глубина центра от переднего стекла
TD_USB_OUT = 1.2                           # Type-C выступает за край платы
TD_WIN_INSET = 1.0                         # окно меньше стекла с каждой стороны (центр окна = центр стекла)
TD_RIB_T, TD_RIB_H = 1.5, 4.9              # рёбра кармана крышки: толщина и высота от лицевой пластины
TD_RIB_Y = (6.0, 58.0)                     # рёбра вдоль длинных сторон: диапазон от края USB
TD_HOOK_Y = (16.0, 54.5)                   # защёлки: положения от края USB (между ножками гребёнок и деталями)
TD_HOOK_W, TD_HOOK_SLIT = 5.0, 1.0         # ширина защёлки и прорезь в ребре по бокам от неё
TD_HOOK_OVERLAP = 0.5                      # заход носика на край платы
TD_CATCH_D = 6.5                           # глубина упора носика (тыл платы на 6.38)
TD_CLIP_T = 1.0                            # защёлка (отдельная деталь, печать плашмя): толщина гибкой лапки
TD_CLIP_BASE_D = 1.4                       # глубина основания в гнезде крышки (остаток лицевой пластины 0.8)
TD_CLIP_BASE_IN = 0.5                      # основание: заход внутрь (под край модуля) от лапки
TD_CLIP_GAP = 1.0                          # зазор в гнезде снаружи лапки — лапка гнётся от дна гнезда
TD_CLIP_FIT = 0.12                         # зазор основания в гнезде под клей (ацетон / ABS-клей)
# Кнопки BOOT (IO0) и IO14: на лицевой стороне платы по бокам от USB-C, утоплены относительно стекла (по STEP).
# Нажимаются штырьками (TDisplayS3_buttons_x2.stl) через отверстия в крышке; буртик изнутри — не выпадают наружу.
TD_BTN_XY = ((-9.29, 2.32), (8.81, 2.20))  # центры толкателей: x поперёк платы, y от края USB
TD_BTN_TOP_D = 2.68                        # верх толкателя от лицевой поверхности стекла (= внутр. плоскость крышки)
TD_BTN_BODY_D = 3.68                       # верх корпуса кнопки (толкатель 2.5 x 1.8 выступает на 1 мм)
TD_BTN_HOLE_D = 3.0                        # отверстие в крышке
TD_BTN_PIN_D = 2.6                         # стержень штырька
TD_BTN_PIN_OUT = 1.0                       # выступ штырька над крышкой
TD_BTN_COLLAR_D = 4.2                      # буртик под крышкой (не даёт выпасть наружу)
TD_BTN_TIP_D = 1.6                         # наконечник на толкатель кнопки (снизу конус 45° — печать без поддержек)
TD_BTN_GAP = 0.2                           # зазор наконечника до толкателя (без нажатия)
TD_CLR_SIDE = 1.5                          # зазор по бокам платы K22 (определяет ширину корпуса)
TD_NRF_X = 0.0                             # nRF24 по X
TD_NRF_VERTICAL = True                     # nRF24 длинной стороной вдоль Y (антенна вверх): освобождает место для шурупов
TD_CABLE_X = -22.0                         # заужение под провод по X (левее, не задевая нижний блок)
TD_BOT_SLOT_HALF = 17.0                    # нижние щели: симметрично, в пределах ±X
TD_WALLHOLE_X = 24.0                       # заужения под шурупы в стену: ±X, по Y — на уровне центра экрана (сбоку от nRF24)

# --- датчик K22 (Docs/K22 size.png, K22-PWM; K22-OC — проверить штангенциркулем) ---
K22_L, K22_W, K22_T = 64.8, 59.7, 1.64     # плата K22: длина, ширина, толщина
K22_HEIGHT = 35.0                          # максимальная высота над платой
K22_HOLES = [(7.4, 8.0), (7.4, 35.0), (57.4, 8.0), (57.4, 35.0)]   # от левого нижнего угла платы
K22_HOLE_D = 4.1                           # отверстия платы K22 (справочно)
K22_STAND_H = 3.0                          # высота стоек под платой K22
# Крепёж: плата K22 -> 4 стойки на дне, винт M4 × 6 (саморез по пластику), макс. длина 6.2 = 1.64 + 4.6
K22_POST_D, K22_POST_PILOT = 7.2, 3.4      # стойки K22: диаметр стойки, отверстие под саморез M4
K22_PILOT_BOTTOM = 0.8                     # дно отверстия от наружной поверхности дна (снаружи остаётся 0.8 мм)
K22_CLEAR_TOP = 2.0                        # зазор над камерой K22

# --- датчик SenseAir S8 LP (вариант TDisplayS3_S8; Docs/Senseair S8 LP product specification.md) ---
# Приоритет — монтаж НА СТЕНУ (крышка со щелями). Датчик наклейкой к дну, плата с окном диффузии — к крышке,
# ПОДНЯТ к крышке (до неё S8_TOP_GAP, над окном — щели крышки), по X — по центру, длинной стороной вдоль X;
# выводы — на коротких торцах (UART — 5 выводов). Стоит на 2 тонких рёбрах со дна (поперёк датчика, на концах —
# направляющие по Y), сверху прижат 2 штырьками крышки — мимо окна диффузии и площадок выводов. Винтов нет.
# Окно и площадки — по рисунку 3 спецификации (вид на плату; размеров там нет, точность ~±0,5 мм).
# Координаты на датчике: u — вдоль, от центра к торцу UART; v — поперёк (на рисунке вверх);
# в корпусе X = S8_UART_SIDE * u, Y = S8_YC + S8_UART_SIDE * v.
S8_L, S8_W, S8_H = 33.9, 19.8, 8.7         # максимальный габарит (спецификация)
S8_UART_SIDE = 1                           # торец с выводами UART: +1 — справа (+X), как на рисунке выводов (вид на плату)
S8_TOP_GAP = 5.0                           # от платы датчика (окно диффузии) до внутренней плоскости крышки
S8_WALL_GAP = 3.0                          # от нижней стенки до направляющих — воздух от нижних щелей к окну
S8_WIN_UV = ((-12.9, -4.3), (-3.4, -4.3), (4.9, 1.8), (4.9, 9.0), (-12.9, 9.0))   # контур окна диффузии (u, v)
S8_PAD_U = 15.2                            # площадки выводов: u = ± это (провода — над ними)
S8_PIN_UV = ((9.5, 0.0), (-9.0, -7.1))     # штырьки крышки (u, v): между окном и выводами UART; под окном
S8_PIN_D = 3.0                             # диаметр штырька
S8_PIN_PRESS = 0.2                         # натяг штырька на датчик
S8_PIN_WALL = 0.8                          # щели крышки обходят штырёк с этим запасом
S8_RIB_T = 1.6                             # опорные рёбра (по u штырьков — нагрузка прямо в ребро): толщина
S8_GUIDE_L, S8_GUIDE_T, S8_GUIDE_H = 6.0, 1.6, 3.0   # направляющие на концах рёбер: длина по X, толщина, выше опоры
S8_GUIDE_CLR = 0.3                         # зазор направляющих до датчика
S8_PIN_ZONE = 4.0                          # макет: выводы у торцов (от торца)
S8_ZONE_GAP = 3.0                          # между зоной датчика и зоной экрана
S8_TOP_CLR = 1.0                           # от направляющих до верха зоны датчика
S8_NRF_TOP_CLR = 1.0                       # глубина корпуса: зазор над nRF24 до деталей на тыле платы экрана

# --- уши-прижимы (вариант TDisplayS3_S8): плоские планки с отверстием под M3, печать плашмя ---
# Крепёж: плата T-Display -> 4 стойки на крышке, саморез M3 × 6 (макс. длина 7.5 = EAR_T + TD_EAR_PILOT_DEPTH)
EAR_W = 6.0                                # ширина уха платы T-Display
EAR_T = 2.5                                # толщина уха
EAR_HOLE_D = 3.4                           # сквозное отверстие под M3
TD_EAR_OVERLAP = 0.5                       # заход уха на край платы T-Display (как носик защёлки)
TD_EAR_POST_D = 6.0                        # стойки ушей платы на крышке (там, где были защёлки: TD_HOOK_Y)
TD_EAR_POST_X = 18.0                       # ось стойки от оси платы (поперёк), снаружи рёбер кармана
TD_EAR_PILOT, TD_EAR_PILOT_DEPTH = 2.6, 5.0     # отверстие в стойке под саморез M3 и его глубина

# --- модуль nRF24L01+ (стандартный, как на фото: антенна на одном конце, кварц вдоль края, разъём 2x4 на другом) ---
# Стоит НА ДЛИННОМ РЕБРЕ на дне (плата перпендикулярна дну), пины разъёма — вбок (удобно надеть провода).
# Крепёж: винтов нет — плата держится в стойках (при необходимости капля термоклея у основания)
# Держится в двух парах одинаковых стоек (по одной с каждой стороны платы). Положения — по типовому модулю
# (s — от края со стороны антенны вдоль длины, высота — от нижнего, опорного края платы).
NRF_L, NRF_W, NRF_T = 29.0, 16.0, 1.0      # длина, ширина (= высота стоя), толщина платы
NRF_X = -27.0                              # ES3C28P: центр модуля по X (антенна — к левой стене)
NRF_SLOT = 2.5                             # зазор между стойками пары (плата 1 мм + место под выводы деталей)
NRF_POST_T = 2.5                           # толщина стойки (поперёк платы)
NRF_POST_W = 4.0                           # ширина стойки (вдоль модуля)
NRF_HOLD_A_S = 10.5                        # пара A (ближе к стенке): между антенной и кварцем (центр, s)
NRF_HOLD_A_H = 10.0                        #   высота
NRF_HOLD_B_S = 26.5                        # пара B: у разъёма (центр, s)
NRF_HOLD_B_H = 4.0                         #   высота: от нижнего края платы до разъёма
NRF_STOP_H = 4.0                           # задняя стойка-упор у торца со стороны разъёма (модуль не сползает)
NRF_STOP_T = 2.5                           #   её толщина (вдоль модуля), как у всех стоек
NRF_CHAMFER = 0.5                          # скос 45° верхнего внутреннего ребра стоек (заход платы)
NRF_PARTS_H = 4.0                          # макет: высота деталей (кварц) над платой со стороны деталей
NRF_WIRES_L = 23.0                         # макет: пины 8.5 + разъёмы Dupont 14.5 от платы со стороны пинов
NRF_HEADER_L = 5.5                         # макет: зона разъёма вдоль модуля от торца

# --- вентиляция корпуса (узкие щели — меньше пыли; щели вертикальные — без мостов при печати) ---
SLOT_W = 1.2                               # ширина щели
SLOT_PITCH = 3.4                           # шаг щелей
SIDE_SLOT_Y = (11.0, 49.0)                 # боковые щели: диапазон по Y от низа платы K22
BOT_SLOT_LEN = 28.0                        # щели нижней стенки (вдоль Z)
BOT_SLOT_HALF = 26.0                       # ES3C28P: щели нижней стенки симметрично, в пределах ±X
LIDVENT_X = 31.0                           # щели крышки «на стену»: в пределах ±X
LIDVENT_Y_MARGIN = 3.0                     # щели крышки: отступ от краёв платы K22 (зоны S8) по Y
S8_LIDVENT_EXTRA = 6.0                     # S8: щели крышки по X — на столько шире датчика с каждой стороны
S8_BOT_SLOT_DOWN = 1.0                     # S8: нижние щели — от низа датчика минус это...
S8_BOT_SLOT_TOP = 1.0                      #   ...до (верх стенки - это): к зазору над окном; юбка крышки над ними вырезана

# --- заужения под сверление: с ВНУТРЕННЕЙ стороны стенок (снаружи гладко), остаётся мембрана ---
MEMBRANE = 0.8                             # остаточная толщина мембраны (снаружи)
CABLE_X = -33.0                            # ES3C28P: провод — заужение в нижней стенке у левой боковины
CABLE_D = 8.0                              # диаметр заужения (каплевидная форма — без поддержек)
CABLE_Z_OFF = 12.0                         # высота центра от внутренней поверхности дна
WALLHOLE_X = 38.5                          # ES3C28P: заужения под шурупы в стену, ±X (по Y — между K22 и nRF24)
# Крепёж: корпус -> стена, 2 шурупа 3.5–4 × 25–30 мм с полукруглой / прессшайбой головкой (Ø головки ≥ 7) + дюбели 5–6 мм
WALLHOLE_D = 4.1                           # отверстие под шуруп в стену: на всю глубину до мембраны (проколоть шурупом / сверлить Ø4)

# --- крепление крышки: 4 винта M3 в угловых блоках (все 4 одинаковые) ---
# Крепёж: крышка -> корпус, 4 винта M3 × 12…16 с потайной головкой (DIN 965 / ISO 7046 или саморез потай 3 × 12…16);
#         макс. длина 16.2 = крышка 2.2 + глубина отверстия 14
BOSS_PILOT, BOSS_DEPTH = 2.6, 14.0         # отверстие в угловом блоке под саморез M3 и его глубина от плоскости крышки
BLOCK_W, BLOCK_H, BLOCK_R = 7.0, 6.5, 2.5  # блок в углу: прямоугольник, сросшийся с двумя стенками, один угол скруглён
CLR_TOP_PCB = 0.6                          # зазор между зоной экрана и верхним блоком
LID_HOLE_D, LID_CSK_D = 3.4, 6.6           # сквозное отверстие в крышке под M3 и диаметр потайной фаски
LID_CSK_H = 1.8                            # глубина потайной фаски (головка M3 потай ~1.7 мм)
LIP_T, LIP_H, LIP_CLR = 1.2, 2.5, 0.25     # юбка крышки, заходящая в корпус
LIP_R = 0.8                                # радиус углов юбки (меньше R_IN)

# --- вертикальная компоновка ---
CLR_LOW = 1.5                              # зазор под K22
ZONE_GAP = 5.0                             # между K22 и зоной экрана
# ======================================================================================

# --- расчёт размеров (зависит от выбранной платы) ---
ZH = TD_GLASS_W / 2 + TD_RIB_T + 0.3 - 0.3 + 0.28   # T-Display: половина высоты зоны экрана (рёбра 13.28 + толщина)
ZH = TD_LCD_W / 2 + 0.3 + TD_RIB_T                  # = 14.78
TD_S = 1 if USB_SIDE == "left" else -1              # T-Display: знак поворота платы в корпус
TD_Y_CENTER = TD_GLASS_Y0 + TD_GLASS_L / 2          # центр стекла вдоль длины платы (от края USB)

INNER_W = (K22_L + 2 * TD_CLR_SIDE) if IS_TD else (PCB_L + 2 * CLR_SIDE)
W = INNER_W + 2 * WALL
BLOCK_X0 = INNER_W / 2 - BLOCK_W           # внутренняя грань углового блока (по модулю X)
BLOCK_Y1_LOW = WALL + BLOCK_H              # верхняя грань нижнего блока
# нижние блоки стоят рядом с K22 (ES3C28P) или под ним (T-Display: корпус уже платы K22 + блока)
K22_Y0 = WALL + CLR_LOW if BLOCK_X0 >= K22_L / 2 + 0.5 else BLOCK_Y1_LOW + 0.6
K22_YC = K22_Y0 + K22_W / 2
# S8: зона датчика по Y — зазор у нижней стенки, направляющие рёбер с обеих длинных сторон
SENS_Y0 = (WALL + S8_WALL_GAP) if IS_S8 else K22_Y0      # низ зоны датчика
S8_GUIDE_YO = S8_W / 2 + S8_GUIDE_CLR                     # внутренняя грань направляющих от оси датчика
S8_YC = SENS_Y0 + S8_GUIDE_T + S8_GUIDE_YO                # ось датчика S8 по Y
SENS_Y1 = (S8_YC + S8_GUIDE_YO + S8_GUIDE_T + S8_TOP_CLR) if IS_S8 else (K22_Y0 + K22_W)   # верх зоны датчика
if IS_S8:   # зона экрана выше: стойки ушей платы снаружи рёбер кармана
    ZH = TD_EAR_POST_X + TD_EAR_POST_D / 2 + 0.5
if IS_TD:
    YD = SENS_Y1 + (S8_ZONE_GAP if IS_S8 else ZONE_GAP + 0.1) + ZH
    H = YD + ZH + CLR_TOP_PCB + BLOCK_H + WALL
    PCB_Y0 = YD - TD_PCB_W / 2
else:
    PCB_Y0 = K22_Y0 + K22_W + ZONE_GAP + CLR_PCB
    YD = PCB_Y0 + PCB_W / 2                # центр платы экрана
    H = PCB_Y0 + PCB_W + CLR_TOP_PCB + BLOCK_H + WALL
if IS_S8:   # глубина: nRF24 на ребре + зазор + стек платы экрана (стекло -> тыльные детали)
    INNER_D = math.ceil(NRF_W + S8_NRF_TOP_CLR + TD_STACK_H + TD_PCB_T + TD_BACK_H)
else:
    INNER_D = K22_STAND_H + K22_T + K22_HEIGHT + K22_CLEAR_TOP
ZL = BACK_T + INNER_D                      # внутренняя плоскость крышки (верх стенок)
S8_Z0 = ZL - S8_TOP_GAP - S8_H             # S8: низ датчика (верх опорных рёбер)
D = ZL + LID_T
Z_PCB = ZL - POST_H                        # ES3C28P: лицевая сторона платы экрана
BLOCK_Y0_TOP = H - WALL - BLOCK_H          # нижняя грань верхнего блока
BLOCK_XC = INNER_W / 2 - BLOCK_W / 2       # ось винта по X (по модулю)
SCREWS = ([(sx * BLOCK_XC, WALL + BLOCK_H / 2) for sx in (-1, 1)] +
          [(sx * BLOCK_XC, BLOCK_Y0_TOP + BLOCK_H / 2) for sx in (-1, 1)])
ES_WIN_L = 2 * (VA_OFFSET_X + VA_L / 2 + VA_MARGIN)   # окно ES3C28P по центру, с запасом на смещение экрана
ES_WIN_W = VA_W + 2 * VA_MARGIN
TD_WIN_L, TD_WIN_W = TD_GLASS_L - 2 * TD_WIN_INSET, TD_GLASS_W - 2 * TD_WIN_INSET
NRF_X_A = TD_NRF_X if IS_TD else NRF_X
CABLE_X_A = TD_CABLE_X if IS_TD else CABLE_X
BOT_HALF_A = TD_BOT_SLOT_HALF if IS_TD else BOT_SLOT_HALF
WALLHOLE_X_A = TD_WALLHOLE_X if IS_TD else WALLHOLE_X
NRF_VERT = IS_TD and TD_NRF_VERTICAL                  # nRF24 длинной стороной вдоль Y
# Система координат модуля: s — вдоль длины от торца антенны, n — по нормали к плате от её средней плоскости
# (+n — сторона пинов разъёма), h — высота над дном. ES3C28P: длина вдоль X (антенна слева), пины вверх (+Y,
# дальше от K22 и шурупов). T-Display: длина вдоль Y (антенна вверху), пины вправо (+X).
NRF_ORG = (NRF_X_A, YD + NRF_L / 2) if NRF_VERT else (NRF_X_A - NRF_L / 2, YD)
NRF_AX_S = (0, -1) if NRF_VERT else (1, 0)
NRF_AX_N = (1, 0) if NRF_VERT else (0, 1)
NRF_HALF_SLOT = NRF_SLOT / 2
NRF_HOLD_N = NRF_HALF_SLOT + NRF_POST_T               # габарит держателей по нормали от средней плоскости
# шурупы в стену: при nRF24 вдоль Y — на уровне центра экрана сбоку от модуля, иначе — посередине между
# верхним краем K22 и зоной модуля (как при прежних направляющих: YD - NRF_W / 2 - 1.5)
WALLHOLE_Y = YD if NRF_VERT else (K22_Y0 + K22_W + YD - NRF_W / 2 - 1.5) / 2


def S(shape):
    return cq.Workplane("XY").newObject([shape])


def cyl(x, y, d, z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(d / 2).extrude(z1 - z0)


def rbox(cx, cy, w, h, z0, z1, r):
    return cq.Workplane("XY").workplane(offset=z0).center(cx, cy).rect(w, h).extrude(z1 - z0).edges("|Z").fillet(r)


def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def for_print(wp):
    """Деталь для STL: масштаб (1 + SHRINK_XY) по X, Y и (1 + SHRINK_Z) по Z — компенсация усадки.
    Вызывать после поворота в положение печати (плоскость XY = стол). Модель и проверки — в номинале."""
    if SHRINK_XY == 0 and SHRINK_Z == 0:
        return wp
    from OCP.gp import gp_GTrsf, gp_Mat
    from OCP.BRepBuilderAPI import BRepBuilderAPI_GTransform
    kxy, kz = 1 + SHRINK_XY, 1 + SHRINK_Z
    g = gp_GTrsf()
    g.SetVectorialPart(gp_Mat(kxy, 0, 0, 0, kxy, 0, 0, 0, kz))
    return S(cq.Shape.cast(BRepBuilderAPI_GTransform(wp.val().wrapped, g, True).Shape()))


def cone(x, y, z0, r0, z1, r1):
    return S(cq.Solid.makeCone(r0, r1, abs(z1 - z0), cq.Vector(x, y, z0), cq.Vector(0, 0, 1)))


def td_xy(x, y):
    """T-Display: координаты платы (x поперёк, y от края USB) -> (X, Y) корпуса; центр стекла в X = 0."""
    return TD_S * (y - TD_Y_CENTER), YD + TD_S * x


def tdbox(x0, x1, y0, y1, d0, d1):
    """Параллелепипед в координатах платы T-Display; d — глубина от переднего стекла (к телу корпуса)."""
    xa, ya = td_xy(x0, y0)
    xb, yb = td_xy(x1, y1)
    return box(min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb), ZL - d1, ZL - d0)


def nrf_xy(s, n):
    """Модуль nRF24: (s вдоль длины от антенны, n по нормали к плате) -> (X, Y) корпуса."""
    return (NRF_ORG[0] + s * NRF_AX_S[0] + n * NRF_AX_N[0], NRF_ORG[1] + s * NRF_AX_S[1] + n * NRF_AX_N[1])


def nrf_box(s0, s1, n0, n1, h0, h1):
    """Параллелепипед в координатах модуля nRF24; h — высота над внутренней поверхностью дна."""
    xa, ya = nrf_xy(s0, n0)
    xb, yb = nrf_xy(s1, n1)
    return box(min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb), BACK_T + h0, BACK_T + h1)


def nrf_rect(s0, s1, n0, n1):
    """Прямоугольник в координатах модуля -> (x0, x1, y0, y1) корпуса (для чертежа и проверок)."""
    xa, ya = nrf_xy(s0, n0)
    xb, yb = nrf_xy(s1, n1)
    return min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb)


def rect_dist(x, y, r):
    """Расстояние от точки до прямоугольника (x0, x1, y0, y1); 0 — внутри."""
    dx = max(r[0] - x, 0, x - r[1])
    dy = max(r[2] - y, 0, y - r[3])
    return math.hypot(dx, dy)


def nrf_holders():
    """Держатели модуля, стоящего на ребре: две пары одинаковых стоек (по обе стороны платы) + упор у торца
    разъёма. Стойки растут из дна (h от -0.5 — срастаются с ним); печать дном вниз — без поддержек."""
    hs, pt, pw, c = NRF_HALF_SLOT, NRF_POST_T, NRF_POST_W, NRF_CHAMFER
    vs, vn = cq.Vector(NRF_AX_S[0], NRF_AX_S[1], 0), cq.Vector(NRF_AX_N[0], NRF_AX_N[1], 0)
    parts = []
    # стойки пар: профиль в плоскости (n, h) со скосом у паза, вытянут вдоль s (s x n = +Z — плоскость правая)
    for sc, h in ((NRF_HOLD_A_S, NRF_HOLD_A_H), (NRF_HOLD_B_S, NRF_HOLD_B_H)):
        ox, oy = nrf_xy(sc - pw / 2, 0)
        pl = cq.Plane(origin=(ox, oy, BACK_T), xDir=vn, normal=vs)
        for n_in, sgn in ((hs, 1), (-hs, -1)):                         # обе стороны платы; n_in — грань к пазу
            n_out = n_in + sgn * pt
            pts = [(n_in, -0.5), (n_out, -0.5), (n_out, h), (n_in + sgn * c, h), (n_in, h - c)]
            parts.append(cq.Workplane(pl).polyline(pts).close().extrude(pw))
    # задняя стойка-упор (без скоса)
    parts.append(nrf_box(NRF_L + 0.3, NRF_L + 0.3 + NRF_STOP_T, -NRF_HOLD_N, NRF_HOLD_N, -0.5, NRF_STOP_H))
    return parts


def teardrop_pocket(cx, zc, d):
    """Каплевидное заужение с ВНУТРЕННЕЙ стороны нижней стенки (снаружи гладко, мембрана MEMBRANE).
    Вершина капли вверх (+Z) — без поддержек."""
    r = d / 2
    y_from = WALL + 0.5                       # начало выдавливания (внутри полости)
    depth = y_from - MEMBRANE                 # до мембраны
    k = r * math.sqrt(2)
    c45 = r * math.cos(math.pi / 4)
    circle = cq.Workplane("XZ", origin=(0, y_from, 0)).center(cx, zc).circle(r).extrude(depth)
    tip = (cq.Workplane("XZ", origin=(0, y_from, 0))
           .polyline([(cx - c45, zc + c45), (cx, zc + k), (cx + c45, zc + c45)]).close().extrude(depth))
    return circle.union(tip)


def corner_block(sx, top):
    """Угловой блок для винта крышки: сросшийся с боковой и верхней/нижней стенками, один угол скруглён."""
    xa, xb = (BLOCK_X0, INNER_W / 2 + 0.5) if sx > 0 else (-INNER_W / 2 - 0.5, -BLOCK_X0)
    if top:
        y0, y1, cy, py = BLOCK_Y0_TOP, H - WALL + 0.5, BLOCK_Y0_TOP, BLOCK_Y0_TOP + BLOCK_H / 2
    else:
        y0, y1, cy, py = WALL - 0.5, BLOCK_Y1_LOW, BLOCK_Y1_LOW, WALL + BLOCK_H / 2
    blk = box(xa, xb, y0, y1, BACK_T - 0.5, ZL)
    blk = blk.edges("|Z").edges(cq.selectors.NearestToPointSelector((sx * BLOCK_X0, cy, (BACK_T + ZL) / 2))).fillet(BLOCK_R)
    return blk.cut(cyl(sx * BLOCK_XC, py, BOSS_PILOT, ZL - BOSS_DEPTH, ZL + 1))


def _sym_xs(half):
    n = int(2 * half // SLOT_PITCH)
    return [-n * SLOT_PITCH / 2 + i * SLOT_PITCH for i in range(n + 1)]


def side_slot_ys():
    """Центры боковых щелей по Y: напротив K22 (SIDE_SLOT_Y от низа платы K22) или зоны датчика S8."""
    y0, y1 = (SENS_Y0 + 3.0, SENS_Y1 - 3.0) if IS_S8 else (K22_Y0 + SIDE_SLOT_Y[0], K22_Y0 + SIDE_SLOT_Y[1])
    ys, out = y0, []
    while ys <= y1:
        out.append(ys)
        ys += SLOT_PITCH
    return out


def s8_xy(u, v):
    """Точка на датчике S8 (u — к торцу UART, v — поперёк, как на рисунке 3 спецификации) -> (X, Y) корпуса."""
    return S8_UART_SIDE * u, S8_YC + S8_UART_SIDE * v


def s8_pins():
    """Прижимные штырьки крышки (X, Y)."""
    return [s8_xy(u, v) for u, v in S8_PIN_UV]


def s8_ribs():
    """Опорные рёбра S8 (профиль «I» в плане): стенка поперёк датчика под каждым штырьком (верх — S8_Z0)
    + направляющие по обе длинные стороны (выше опоры на S8_GUIDE_H) — датчик не сдвигается по Y."""
    out = None
    for x, _ in s8_pins():
        r = box(x - S8_RIB_T / 2, x + S8_RIB_T / 2, S8_YC - S8_GUIDE_YO, S8_YC + S8_GUIDE_YO, BACK_T - 0.5, S8_Z0)
        for sy in (-1, 1):
            ya, yb = sorted((S8_YC + sy * S8_GUIDE_YO, S8_YC + sy * (S8_GUIDE_YO + S8_GUIDE_T)))
            r = r.union(box(x - S8_GUIDE_L / 2, x + S8_GUIDE_L / 2, ya, yb, BACK_T - 0.5, S8_Z0 + S8_GUIDE_H))
        out = r if out is None else out.union(r)
    return out


def s8_window(z0, z1):
    """Окно диффузии S8 (контур S8_WIN_UV), призма от z0 до z1 — макет «воздух над окном»."""
    return cq.Workplane("XY").workplane(offset=z0).polyline([s8_xy(u, v) for u, v in S8_WIN_UV]).close().extrude(z1 - z0)


def split_segments(segs, a, b):
    """Вычесть интервал (a, b) из списка отрезков [(y0, y1)]."""
    out = []
    for y0, y1 in segs:
        if b <= y0 or a >= y1:
            out.append((y0, y1))
            continue
        if a > y0:
            out.append((y0, a))
        if b < y1:
            out.append((b, y1))
    return out


def td_ear_posts():
    """Стойки ушей платы T-Display: (x поперёк платы, y от края USB) — там, где были защёлки."""
    return [(sx * TD_EAR_POST_X, yh) for sx in (-1, 1) for yh in TD_HOOK_Y]


def ear(length, hole_off, width=EAR_W):
    """Ухо-прижим: планка length x width x EAR_T вдоль +X от 0, отверстие на hole_off, скруглённый наружный конец.
    Печать плашмя (Z — толщина)."""
    e = (cq.Workplane("XY").center(length / 2, 0).rect(length, width).extrude(EAR_T)
         .edges("|Z").edges(">X").fillet(width / 2 - 0.01))
    return e.cut(cyl(hole_off, 0, EAR_HOLE_D, -1, EAR_T + 1))


TD_EAR_LEN = TD_EAR_POST_X - (TD_PCB_W / 2 - TD_EAR_OVERLAP) + TD_EAR_POST_D / 2


def td_ears_in_place():
    """4 уха платы T-Display: лежат на стойках крышки и тыле платы (глубина TD_STACK_H + TD_PCB_T .. + EAR_T)."""
    out = []
    d0 = TD_STACK_H + TD_PCB_T
    for bx, by in td_ear_posts():
        sx = 1 if bx > 0 else -1
        x_in = sx * (TD_PCB_W / 2 - TD_EAR_OVERLAP)
        e = ear(TD_EAR_LEN, TD_EAR_LEN - TD_EAR_POST_D / 2)
        X0, Y0 = td_xy(x_in, by)
        X1, Y1 = td_xy(bx, by)
        ang = math.degrees(math.atan2(Y1 - Y0, X1 - X0))
        out.append(e.rotate((0, 0, 0), (0, 0, 1), ang).translate((X0, Y0, ZL - d0 - EAR_T)))
    return out


def ears_print():
    """Уши для печати плашмя: 4 уха платы T-Display."""
    parts = [ear(TD_EAR_LEN, TD_EAR_LEN - TD_EAR_POST_D / 2).translate((0, i * (EAR_W + 2), 0)) for i in range(4)]
    res = parts[0]
    for p in parts[1:]:
        res = res.union(p)
    return res


def bottom_slot_xs():
    """Центры щелей нижней стенки: симметрично относительно X = 0."""
    return _sym_xs(BOT_HALF_A)


def lidvent_xs():
    """Центры щелей крышки «на стену»: симметрично относительно X = 0, в пределах ±LIDVENT_X."""
    return _sym_xs(min(LIDVENT_X, S8_L / 2 + S8_LIDVENT_EXTRA) if IS_S8 else LIDVENT_X)


def usb_center():
    """Центр выреза Type-C: (Y, Z)."""
    if IS_TD:
        return YD + TD_S * TD_USB_X, ZL - TD_USB_D
    return YD, Z_PCB + USB_Z_FROM_PCB_FRONT


def build_body():
    body = rbox(0, H / 2, W, H, 0, ZL, R_OUT)
    body = body.cut(rbox(0, H / 2, INNER_W, H - 2 * WALL, BACK_T, ZL + 1, R_IN))

    # угловые блоки под винты крышки: 2 нижних и 2 верхних (одинаковые)
    for sx in (-1, 1):
        for top in (False, True):
            body = body.union(corner_block(sx, top))

    if IS_S8:
        # 2 опорных ребра S8 (датчик поднят к крышке; сверху прижат штырьками крышки)
        body = body.union(s8_ribs())
    else:
        # стойки K22 (центр платы K22 — по X = 0, плата стоит так же, как на чертеже)
        for hx, hy in K22_HOLES:
            x, y = hx - K22_L / 2, K22_YC + hy - K22_W / 2
            body = body.union(cyl(x, y, K22_POST_D, BACK_T - 0.5, BACK_T + K22_STAND_H))
            # отверстие под саморез — сквозь стойку и в дно, до K22_PILOT_BOTTOM от наружной поверхности
            body = body.cut(cyl(x, y, K22_POST_PILOT, K22_PILOT_BOTTOM, BACK_T + K22_STAND_H + 1))

    # держатели nRF24 за экраном (на дне): модуль стоит на ребре, пины вбок
    for p in nrf_holders():
        body = body.union(p)

    # заужения дна под шурупы для крепления к стене (изнутри, открыты вверх — без поддержек):
    # Ø WALLHOLE_D до мембраны
    for sx in (-1, 1):
        body = body.cut(cyl(sx * WALLHOLE_X_A, WALLHOLE_Y, WALLHOLE_D, MEMBRANE, BACK_T + 0.5))

    # боковые щели вдоль Z (вертикальные при печати — без мостов), напротив датчика
    z_start, z_end = BACK_T + 4.0, ZL - 4.0
    for ys in side_slot_ys():
        for sx in (-1, 1):
            xa, xb = (W / 2 - WALL - 0.5, W / 2 + 0.5) if sx > 0 else (-W / 2 - 0.5, -W / 2 + WALL + 0.5)
            body = body.cut(box(xa, xb, ys - SLOT_W / 2, ys + SLOT_W / 2, z_start, z_end))

    # щели нижней стенки (вдоль Z), симметрично от углов; не длиннее глубины корпуса.
    # S8: на уровне датчика и зазора над окном диффузии (воздух снизу идёт вдоль платы датчика к щелям крышки)
    bz0, bz1 = (S8_Z0 - S8_BOT_SLOT_DOWN, ZL - S8_BOT_SLOT_TOP) if IS_S8 else \
        (BACK_T + 4.0, BACK_T + 4.0 + min(BOT_SLOT_LEN, ZL - 4.0 - (BACK_T + 4.0)))
    for x in bottom_slot_xs():
        body = body.cut(box(x - SLOT_W / 2, x + SLOT_W / 2, -0.5, WALL + 0.5, bz0, bz1))

    # заужение под провод: одно, в нижней стенке у левой боковины, с внутренней стороны (каплевидное)
    body = body.cut(teardrop_pocket(CABLE_X_A, BACK_T + CABLE_Z_OFF, CABLE_D))

    # вырез Type-C: на стороне USB
    sx = 1 if USB_SIDE == "right" else -1
    yc, zc = usb_center()
    x0 = (W / 2 - WALL - 1) if sx > 0 else -(W / 2 + 1)
    usb = (cq.Workplane("YZ").workplane(offset=x0).center(yc, zc).rect(USB_W, USB_H)
           .extrude(WALL + 2).edges("|X").fillet(min(USB_W, USB_H) / 2 - 0.01))
    # верх выреза без пологих нависаний (печать — вдоль Z): скругления только до 45°, дальше — касательные
    # под 45° до ровного моста (на стыке пологой дуги и моста были заусенцы). Вырез по углам чуть больше.
    r = min(USB_W, USB_H) / 2
    cy = USB_W / 2 - r                             # центры скруглений по Y (USB_W >= USB_H)
    a = r * math.sqrt(0.5)                         # точка касания под 45°
    yt = cy + a - (r - a)                          # конец касательной на уровне верха выреза
    roof = (cq.Workplane("YZ").workplane(offset=x0)
            .polyline([(yc - cy - a, zc), (yc + cy + a, zc), (yc + cy + a, zc + a), (yc + yt, zc + r),
                       (yc - yt, zc + r), (yc - cy - a, zc + a)]).close().extrude(WALL + 2))
    usb = usb.union(roof)
    if ZL - (zc + USB_H / 2) < USB_MIN_BRIDGE:     # тонкая перемычка над вырезом (T-Display) — паз до верха стенки
        usb = usb.union(box(x0, x0 + WALL + 2, yc - USB_W / 2, yc + USB_W / 2, zc, ZL + 1))
    return body.cut(usb)


def td_clip_profile():
    """Профиль защёлки в координатах (u, d): u — поперёк платы наружу от внутренней грани лапки, d — глубина.
    Основание (в гнезде), гибкая лапка, носик с упором на TD_CATCH_D и скосом 45° для защёлкивания."""
    ov = (TD_LCD_W / 2 + 0.3) - (TD_PCB_W / 2 - TD_HOOK_OVERLAP)     # вылет носика внутрь от лапки
    top = TD_CATCH_D + 1.6
    return [(-TD_CLIP_BASE_IN, -TD_CLIP_BASE_D), (TD_CLIP_T, -TD_CLIP_BASE_D), (TD_CLIP_T, top), (0, top),
            (-ov, TD_CATCH_D + 0.4), (-ov, TD_CATCH_D), (0, TD_CATCH_D), (0, 0), (-TD_CLIP_BASE_IN, 0)]


def td_clips_in_lid():
    """4 защёлки на своих местах в крышке (для проверки пересечений и сборки)."""
    xo = TD_LCD_W / 2 + 0.3
    out = []
    for sx in (-1, 1):
        for yh in TD_HOOK_Y:
            X0, _ = td_xy(0, yh - TD_HOOK_W / 2)
            X1, _ = td_xy(0, yh + TD_HOOK_W / 2)
            pts = [(YD + TD_S * sx * (xo + u), ZL - d) for u, d in td_clip_profile()]
            out.append(cq.Workplane("YZ").workplane(offset=min(X0, X1)).polyline(pts).close().extrude(abs(X1 - X0)))
    return out


def td_clips_print():
    """4 защёлки для печати плашмя (профиль в плоскости слоя -> изгиб вдоль слоёв)."""
    prof = td_clip_profile()
    clips = None
    for i in range(4):
        c = cq.Workplane("XY").polyline([(u + i * 6.0, d) for u, d in prof]).close().extrude(TD_HOOK_W)
        clips = c if clips is None else clips.union(c)
    bb = clips.val().BoundingBox()
    return clips.translate((-bb.xmin, -bb.ymin, 0))


def td_button_pin():
    """Штырёк кнопки T-Display: наконечник в z = 0 (на толкатель), ось +Z — наружу через крышку.
    Профиль вращения: наконечник, конус 45° до буртика (печать наконечником вниз без поддержек), буртик
    (упирается в крышку изнутри), стержень через отверстие, выступ наружу с фаской."""
    rt, rc, rp = TD_BTN_TIP_D / 2, TD_BTN_COLLAR_D / 2, TD_BTN_PIN_D / 2
    tip_h = 0.2
    cone_top = tip_h + (rc - rt)                      # 45°
    l_in = TD_BTN_TOP_D - TD_BTN_GAP                  # от наконечника до внутренней плоскости крышки
    total = l_in + LID_T + TD_BTN_PIN_OUT
    ch = 0.4
    assert cone_top < l_in - 0.4, "буртик штырька слишком тонкий"
    pts = [(0, 0), (rt, 0), (rt, tip_h), (rc, cone_top), (rc, l_in), (rp, l_in), (rp, total - ch), (rp - ch, total), (0, total)]
    return cq.Workplane("XZ").polyline(pts).close().revolve(360, (0, 0, 0), (0, 1, 0))


def td_buttons_in_lid():
    """Штырьки на своих местах (для проверки пересечений и сборки)."""
    out = []
    for bx, by in TD_BTN_XY:
        X, Y = td_xy(bx, by)
        out.append(td_button_pin().translate((X, Y, ZL - TD_BTN_TOP_D + TD_BTN_GAP)))
    return out


def td_buttons_print():
    """2 штырька для печати: наконечником вниз, рядом."""
    pin = td_button_pin()
    return pin.union(pin.translate((TD_BTN_COLLAR_D + 3.0, 0, 0)))


def td_pocket(lid):
    """Карман крышки для T-Display-S3: рёбра по краям модуля и две пары гибких защёлок (без отверстий в плате)."""
    xo = TD_LCD_W / 2 + 0.3                       # внутренняя грань рёбер (от оси платы)
    ys0, ys1 = TD_RIB_Y
    for sx in (-1, 1):
        xa, xb = (xo, xo + TD_RIB_T) if sx > 0 else (-xo - TD_RIB_T, -xo)
        lid = lid.union(tdbox(xa, xb, ys0, ys1, -0.5, TD_RIB_H))
        if IS_S8:                                 # вариант с ушами: защёлок нет, рёбра сплошные
            continue
        for yh in TD_HOOK_Y:                      # место под вклеиваемую защёлку: разрыв ребра + глухое гнездо
            hw = TD_HOOK_W / 2
            lid = lid.cut(tdbox(xa, xb, yh - hw - TD_HOOK_SLIT, yh + hw + TD_HOOK_SLIT, 0, TD_RIB_H + 1))
            u0, u1 = -TD_CLIP_BASE_IN - TD_CLIP_FIT, TD_CLIP_T + TD_CLIP_GAP
            x0, x1 = (xo + u0, xo + u1) if sx > 0 else (-xo - u1, -xo - u0)
            lid = lid.cut(tdbox(x0, x1, yh - hw - TD_CLIP_FIT, yh + hw + TD_CLIP_FIT, -TD_CLIP_BASE_D, 0.01))
    # ребро у дальнего от USB торца
    lid = lid.union(tdbox(-xo - TD_RIB_T, xo + TD_RIB_T, TD_PCB_L + 0.3, TD_PCB_L + 0.3 + TD_RIB_T, -0.5, TD_RIB_H))
    if IS_S8:   # стойки ушей платы: от крышки до тыла платы, отверстие под саморез M3 с торца стойки
        d0 = TD_STACK_H + TD_PCB_T
        for bx, by in td_ear_posts():
            X, Y = td_xy(bx, by)
            lid = lid.union(cyl(X, Y, TD_EAR_POST_D, ZL - d0, ZL + 0.5))
            lid = lid.cut(cyl(X, Y, TD_EAR_PILOT, ZL - d0 - 1, ZL - d0 + TD_EAR_PILOT_DEPTH))
    return lid


def build_lid(vents):
    lid = rbox(0, H / 2, W, H, ZL, D, R_OUT)

    if IS_TD:
        win = rbox(0, YD, TD_WIN_L, TD_WIN_W, ZL - 1, D + 1, 1.0)           # по центру стекла
        lid = td_pocket(lid).cut(win)
        for bx, by in TD_BTN_XY:                                            # отверстия под штырьки кнопок
            X, Y = td_xy(bx, by)
            lid = lid.cut(cyl(X, Y, TD_BTN_HOLE_D, ZL - 1, D + 1))
    else:
        win = rbox(0, YD, ES_WIN_L, ES_WIN_W, ZL - 1, D + 1, 1.0)           # по центру, с запасом на смещение VA
        gl, gw = GLASS_L + 2 * GLASS_FIT, GLASS_W + 2 * GLASS_FIT
        ring = rbox(0, YD, gl + 2 * GLASS_RING_T, gw + 2 * GLASS_RING_T, ZL - GLASS_H + 0.3, ZL, 1.5)
        ring = ring.cut(rbox(0, YD, gl, gw, ZL - GLASS_H, ZL + 1, 0.3))
        for sx in (-1, 1):            # вырезы в углах: внутренний угол печатается скруглённым (сопло), стекло — прямоугольное
            for sy in (-1, 1):
                ring = ring.cut(cyl(sx * gl / 2, YD + sy * gw / 2, CORNER_RELIEF_D, ZL - GLASS_H - 1, ZL))
        # сторона USB: шлейфы экрана и тача — рамка ниже, полная высота только у углов
        fx = (-1 if USB_SIDE == "left" else 1) * (gl / 2 + GLASS_RING_T / 2)
        ring = ring.cut(box(fx - GLASS_RING_T, fx + GLASS_RING_T,
                            YD - GLASS_W / 2 + GLASS_FLEX_FULL_BOT, YD + GLASS_W / 2 - GLASS_FLEX_FULL_TOP,
                            ZL - GLASS_H - 1, ZL - GLASS_FLEX_H))
        lid = lid.union(ring).cut(win)
        for sx in (-1, 1):                                                  # стойки платы экрана
            for sy in (-1, 1):
                x, y = sx * HOLE_DX / 2, YD + sy * HOLE_DY / 2
                post = cyl(x, y, POST_D, Z_PCB, ZL).cut(cyl(x, y, POST_PILOT, Z_PCB - 1, Z_PCB + POST_PILOT_DEPTH))
                lid = lid.union(post)

    # юбка, заходящая в корпус (с вырезами под угловые блоки)
    lw, lh = INNER_W - 2 * LIP_CLR, H - 2 * WALL - 2 * LIP_CLR
    lip = rbox(0, H / 2, lw, lh, ZL - LIP_H, ZL, LIP_R)
    lip = lip.cut(rbox(0, H / 2, lw - 2 * LIP_T, lh - 2 * LIP_T, ZL - LIP_H - 1, ZL + 1, 0.3))
    for sx in (-1, 1):
        xa, xb = (BLOCK_X0 - 0.8, INNER_W / 2 + 1) if sx > 0 else (-INNER_W / 2 - 1, -BLOCK_X0 + 0.8)
        lip = lip.cut(box(xa, xb, BLOCK_Y0_TOP - 0.8, H, ZL - LIP_H - 1, ZL + 1))
        lip = lip.cut(box(xa, xb, 0, BLOCK_Y1_LOW + 0.8, ZL - LIP_H - 1, ZL + 1))
    # окно в юбке напротив Type-C — только если вырез достаёт до юбки (T-Display: разъём на стороне экрана;
    # ES3C28P: разъём на тыльной стороне платы, до юбки не доходит — окна нет)
    uyc, uzc = usb_center()
    if uzc + USB_H / 2 > ZL - LIP_H:
        usx0, usx1 = (INNER_W / 2 - LIP_CLR - LIP_T - 1, INNER_W / 2 + 1) if USB_SIDE == "right" else (-INNER_W / 2 - 1, -INNER_W / 2 + LIP_CLR + LIP_T + 1)
        lip = lip.cut(box(usx0, usx1, uyc - USB_W / 2 - 1.0, uyc + USB_W / 2 + 1.0, ZL - LIP_H - 1, ZL + 1))
    if IS_S8:   # нижние щели доходят почти до верха стенки — юбка напротив них вырезана
        xs = bottom_slot_xs()
        lip = lip.cut(box(xs[0] - 2.0, xs[-1] + 2.0, 0, WALL + LIP_CLR + LIP_T + 0.5, ZL - LIP_H - 1, ZL + 1))
    lid = lid.union(lip)

    # щели над K22 / S8 (версия «на стену»): вертикальные сквозные, без мостов; S8 — в обход штырьков
    if vents:
        for x in lidvent_xs():
            segs = [(SENS_Y0 + LIDVENT_Y_MARGIN, SENS_Y1 - LIDVENT_Y_MARGIN)]
            for px, py in (s8_pins() if IS_S8 else []):
                if abs(x - px) < S8_PIN_D / 2 + S8_PIN_WALL + SLOT_W / 2:
                    segs = split_segments(segs, py - S8_PIN_D / 2 - S8_PIN_WALL, py + S8_PIN_D / 2 + S8_PIN_WALL)
            for y0, y1 in segs:
                if y1 - y0 >= 2.0:
                    lid = lid.cut(box(x - SLOT_W / 2, x + SLOT_W / 2, y0, y1, ZL - 1, D + 1))
    if IS_S8:   # прижимные штырьки S8: от крышки до платы датчика (с натягом S8_PIN_PRESS)
        for x, y in s8_pins():
            lid = lid.union(cyl(x, y, S8_PIN_D, S8_Z0 + S8_H - S8_PIN_PRESS, ZL + 0.5))

    # отверстия под винты M3 с потайной головкой
    for x, y in SCREWS:
        lid = lid.cut(cyl(x, y, LID_HOLE_D, ZL - 1, D + 1))
        lid = lid.cut(cone(x, y, D - LID_CSK_H, LID_HOLE_D / 2, D + 0.01, LID_CSK_D / 2))
    return lid


def dummies():
    """Габаритные макеты (для проверки пересечений и сборки)."""
    k22 = box(-K22_L / 2, K22_L / 2, K22_Y0, K22_Y0 + K22_W, BACK_T + K22_STAND_H, BACK_T + K22_STAND_H + K22_T + K22_HEIGHT)
    # nRF24 на ребре: плата + детали (кварц) со стороны -n; провода с разъёмами Dupont со стороны пинов (+n)
    nrf = nrf_box(0, NRF_L, -NRF_T / 2, NRF_T / 2, 0, NRF_W)
    nrf_parts = nrf_box(NRF_HOLD_A_S + NRF_POST_W / 2 + 0.3, NRF_HOLD_B_S - NRF_POST_W / 2 - 0.3,
                        -NRF_T / 2 - NRF_PARTS_H, -NRF_T / 2, 0.5, NRF_W - 0.5)
    nrf_wires = nrf_box(NRF_L - NRF_HEADER_L, NRF_L, NRF_T / 2, NRF_T / 2 + NRF_WIRES_L, NRF_HOLD_B_H + 0.3, NRF_W)
    if IS_S8:   # S8 вместо K22: корпус датчика, провода над выводами на торцах (вбок в зазоре до крышки),
        #         «воздух» над окном диффузии — ничто не должно его перекрывать
        zt = S8_Z0 + S8_H
        sens = {"s8": box(-S8_L / 2, S8_L / 2, S8_YC - S8_W / 2, S8_YC + S8_W / 2, S8_Z0, zt),
                "s8_wires": box(-S8_L / 2, -S8_L / 2 + S8_PIN_ZONE, S8_YC - S8_W / 2, S8_YC + S8_W / 2, zt, ZL - 0.5)
                .union(box(S8_L / 2 - S8_PIN_ZONE, S8_L / 2, S8_YC - S8_W / 2, S8_YC + S8_W / 2, zt, ZL - 0.5)),
                "s8_window": s8_window(zt, ZL - 0.01)}
    else:
        sens = {"k22": k22}
    if IS_TD:
        pcb_front = TD_STACK_H
        pcb_back = TD_STACK_H + TD_PCB_T
        return {
            **sens,
            "pcb": tdbox(-TD_PCB_W / 2, TD_PCB_W / 2, 0, TD_PCB_L, pcb_front, pcb_back),
            "lcd": tdbox(-TD_LCD_W / 2, TD_LCD_W / 2, TD_LCD_Y0, TD_PCB_L, 0, pcb_front),
            "glass": tdbox(-TD_GLASS_W / 2, TD_GLASS_W / 2, TD_GLASS_Y0, TD_GLASS_Y0 + TD_GLASS_L, 0, 1.53),
            "pcb_parts": tdbox(-11.49, 11.49, 0.3, 53.5, pcb_back, pcb_back + TD_BACK_H),
            "usb": tdbox(-4.61, 4.34, -TD_USB_OUT, TD_LCD_Y0, 2.02, 5.18),
            "nrf24": nrf, "nrf_parts": nrf_parts, "nrf_wires": nrf_wires,
            # кнопки: толкатель 2.5 x 1.8 и корпус 3.5 x 3.4 (по STEP), до лица платы
            **{"btn%d" % (i + 1): tdbox(bx - 1.25, bx + 1.25, by - 0.9, by + 0.9, TD_BTN_TOP_D, pcb_front)
               .union(tdbox(bx - 1.75, bx + 1.75, by - 1.7, by + 1.7, TD_BTN_BODY_D, pcb_front))
               for i, (bx, by) in enumerate(TD_BTN_XY)},
        }
    pcb = box(-PCB_L / 2, PCB_L / 2, PCB_Y0, PCB_Y0 + PCB_W, Z_PCB - PCB_T, Z_PCB)
    parts = box(-PCB_L / 2 + 6, PCB_L / 2 - 6, PCB_Y0 + 4, PCB_Y0 + PCB_W - 4, Z_PCB - PCB_T - BACK_PARTS_H, Z_PCB - PCB_T)
    glass = box(-GLASS_L / 2, GLASS_L / 2, YD - GLASS_W / 2, YD + GLASS_W / 2, Z_PCB, Z_PCB + GLASS_H)
    return {"k22": k22, "pcb": pcb, "pcb_parts": parts, "glass": glass,
            "nrf24": nrf, "nrf_parts": nrf_parts, "nrf_wires": nrf_wires}


def volume_of(a, b):
    x = a.intersect(b)
    return sum(s.Volume() for s in x.solids().vals())


# ------------------------------------ чертёж и предпросмотр ------------------------------------
def layout(path):
    """Плоский чертёж: вид спереди (крышка снята) и разрез по Y-Z. Размеры — из параметров."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.patches import Circle, Rectangle
    except ImportError:
        print("layout: matplotlib не установлен — пропуск")
        return

    def rect(ax, x0, x1, y0, y1, **kw):
        kw.setdefault("fill", False)
        ax.add_patch(Rectangle((x0, y0), x1 - x0, y1 - y0, **kw))

    def trect(ax, x0, x1, y0, y1, **kw):          # прямоугольник в координатах платы T-Display
        xa, ya = td_xy(x0, y0)
        xb, yb = td_xy(x1, y1)
        rect(ax, min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb), **kw)

    fig, (a, b) = plt.subplots(1, 2, figsize=(17, 9.5), dpi=100, gridspec_kw={"width_ratios": [1.3, 1]})

    # ---- вид спереди (X-Y) ----
    rect(a, -W / 2, W / 2, 0, H, ec="k", lw=2)
    rect(a, -INNER_W / 2, INNER_W / 2, WALL, H - WALL, ec="gray", lw=1)
    for sx in (-1, 1):
        xa, xb = (BLOCK_X0, INNER_W / 2) if sx > 0 else (-INNER_W / 2, -BLOCK_X0)
        rect(a, xa, xb, BLOCK_Y0_TOP, H - WALL, ec="purple", lw=1.5)
        rect(a, xa, xb, WALL, BLOCK_Y1_LOW, ec="purple", lw=1.5)
        for yy in (BLOCK_Y0_TOP + BLOCK_H / 2, WALL + BLOCK_H / 2):
            a.add_patch(Circle((sx * BLOCK_XC, yy), BOSS_PILOT / 2, fill=False, ec="purple", ls=":"))
        a.add_patch(Circle((sx * WALLHOLE_X_A, WALLHOLE_Y), WALLHOLE_D / 2, fill=False, ec="r", lw=2))
        a.text(sx * WALLHOLE_X_A, WALLHOLE_Y - WALLHOLE_D / 2 - 1, "в стену Ø%.1f" % WALLHOLE_D,
               color="r", ha="center", va="top", fontsize=7)
        for ys in side_slot_ys():
            a.plot([sx * W / 2] * 2, [ys - SLOT_W / 2, ys + SLOT_W / 2], c="r", lw=2, alpha=.7)
    if IS_S8:
        rect(a, -S8_L / 2, S8_L / 2, S8_YC - S8_W / 2, S8_YC + S8_W / 2, ec="darkorange", lw=2)
        from matplotlib.patches import Polygon
        a.add_patch(Polygon([s8_xy(u, v) for u, v in S8_WIN_UV], closed=True, fc="0.3", alpha=.35, ec="0.2"))
        for x, _ in s8_pins():                    # опорные рёбра и направляющие
            rect(a, x - S8_RIB_T / 2, x + S8_RIB_T / 2, S8_YC - S8_GUIDE_YO, S8_YC + S8_GUIDE_YO, ec="darkorange", ls=":")
            for sy in (-1, 1):
                ya, yb = sorted((S8_YC + sy * S8_GUIDE_YO, S8_YC + sy * (S8_GUIDE_YO + S8_GUIDE_T)))
                rect(a, x - S8_GUIDE_L / 2, x + S8_GUIDE_L / 2, ya, yb, ec="darkorange", lw=1)
        for x, y in s8_pins():
            a.add_patch(Circle((x, y), S8_PIN_D / 2, fill=False, ec="m", lw=1.5))
        for su in (-1, 1):
            for v in (5.2, 2.6, 0, -2.6, -5.1) if su == 1 else (5.2, 2.6, 0, -2.6):   # по рисунку 3
                a.add_patch(Circle(s8_xy(su * S8_PAD_U, v), 0.7, fill=False, ec="goldenrod"))
        a.text(S8_UART_SIDE * (S8_L / 2 - 3), S8_YC - 7.5, "UART", ha="center", va="center", color="darkorange", fontsize=7)
        a.text(s8_xy(-4, 3)[0], s8_xy(-4, 3)[1], "окно\nдиффузии", ha="center", va="center", color="0.15", fontsize=7)
        a.text(0, SENS_Y0 - 1.5, "S8 LP поднят к крышке: рёбра (оранж.), штырьки крышки (пурпурные)",
               ha="center", va="top", color="darkorange", fontsize=7)
    else:
        rect(a, -K22_L / 2, K22_L / 2, K22_Y0, K22_Y0 + K22_W, ec="darkorange", lw=2)
        for hx, hy in K22_HOLES:
            a.add_patch(Circle((hx - K22_L / 2, K22_YC + hy - K22_W / 2), K22_HOLE_D / 2, fill=False, ec="darkorange"))
    if not IS_S8:
        a.text(0, K22_YC - 12, "K22 (камера до %.0f мм)\nщели крышки «на стену» — пунктир" % K22_HEIGHT, ha="center", va="center", color="darkorange")
    for x in lidvent_xs():
        a.plot([x, x], [SENS_Y0 + LIDVENT_Y_MARGIN, SENS_Y1 - LIDVENT_Y_MARGIN], c="darkorange", lw=1, ls="--")
    if IS_TD:
        trect(a, -TD_PCB_W / 2, TD_PCB_W / 2, 0, TD_PCB_L, ec="green", lw=2)
        trect(a, -TD_GLASS_W / 2, TD_GLASS_W / 2, TD_GLASS_Y0, TD_GLASS_Y0 + TD_GLASS_L, ec="green", ls="--")
        rect(a, -TD_WIN_L / 2, TD_WIN_L / 2, YD - TD_WIN_W / 2, YD + TD_WIN_W / 2, ec="b", lw=1.5)
        a.text(0, YD, "окно экрана\n%.1f x %.1f (по центру стекла)" % (TD_WIN_L, TD_WIN_W), ha="center", va="center", color="b")
        xo = TD_LCD_W / 2 + 0.3
        for sx in (-1, 1):
            xa, xb = (xo, xo + TD_RIB_T) if sx > 0 else (-xo - TD_RIB_T, -xo)
            trect(a, xa, xb, TD_RIB_Y[0], TD_RIB_Y[1], ec="teal", lw=1)
            for yh in ([] if IS_S8 else TD_HOOK_Y):
                trect(a, xa, xb, yh - TD_HOOK_W / 2, yh + TD_HOOK_W / 2, ec="m", lw=2)
        trect(a, -xo - TD_RIB_T, xo + TD_RIB_T, TD_PCB_L + 0.3, TD_PCB_L + 0.3 + TD_RIB_T, ec="teal", lw=1)
        if IS_S8:
            for bx, by in td_ear_posts():
                a.add_patch(Circle(td_xy(bx, by), TD_EAR_POST_D / 2, fill=False, ec="m", lw=1.5))
                sx = 1 if bx > 0 else -1
                xa2, xb2 = sorted((sx * (TD_PCB_W / 2 - TD_EAR_OVERLAP), bx + sx * TD_EAR_POST_D / 2))
                trect(a, xa2, xb2, by - EAR_W / 2, by + EAR_W / 2, ec="m", lw=1)
            a.text(0, YD - TD_WIN_W / 2 - 3, "уши платы на стойках крышки (пурпурные) — 4 шт", color="m", ha="center", fontsize=8)
        else:
            a.text(0, YD - TD_WIN_W / 2 - 3, "защёлки (пурпурные) — 4 шт", color="m", ha="center", fontsize=8)
        for bx, by in TD_BTN_XY:                  # кнопки BOOT / IO14: отверстия под штырьки
            X, Y = td_xy(bx, by)
            a.add_patch(Circle((X, Y), TD_BTN_HOLE_D / 2, fill=False, ec="r", lw=1.5))
            a.text(X, Y + (3 if Y < YD else -3), "кнопка", color="r", ha="center", va="center", fontsize=7)
    else:
        rect(a, -PCB_L / 2, PCB_L / 2, PCB_Y0, PCB_Y0 + PCB_W, ec="green", lw=2)
        rect(a, -GLASS_L / 2, GLASS_L / 2, YD - GLASS_W / 2, YD + GLASS_W / 2, ec="green", ls="--")
        rect(a, -ES_WIN_L / 2, ES_WIN_L / 2, YD - ES_WIN_W / 2, YD + ES_WIN_W / 2, ec="b", lw=1.5)
        a.text(0, YD + 8, "окно экрана\n%.1f x %.1f (по центру)" % (ES_WIN_L, ES_WIN_W), ha="center", va="center", color="b")
        for sx in (-1, 1):
            for sy in (-1, 1):
                a.add_patch(Circle((sx * HOLE_DX / 2, YD + sy * HOLE_DY / 2), POST_D / 2, fill=False, ec="green"))
    # nRF24 на ребре (вид сверху — тонкая полоса), держатели, зона проводов со стороны пинов
    rect(a, *nrf_rect(0, NRF_L, -NRF_T / 2, NRF_T / 2), ec="teal", lw=2)
    for sc in (NRF_HOLD_A_S, NRF_HOLD_B_S):
        rect(a, *nrf_rect(sc - NRF_POST_W / 2, sc + NRF_POST_W / 2, -NRF_HOLD_N, NRF_HOLD_N), ec="teal", lw=1)
    rect(a, *nrf_rect(NRF_L - NRF_HEADER_L, NRF_L, NRF_T / 2, NRF_T / 2 + NRF_WIRES_L), ec="teal", ls=":")
    tx, ty = nrf_xy(NRF_L / 2, -6)
    a.text(tx, ty, "nRF24 (на ребре)", ha="center", va="center", color="teal", fontsize=8)
    ux = (W / 2) if USB_SIDE == "right" else -W / 2
    uy = usb_center()[0]
    a.plot([ux, ux], [uy - USB_W / 2, uy + USB_W / 2], c="m", lw=6, alpha=.6)
    a.text(ux + (-3 if ux > 0 else 3), uy - USB_W / 2 - 4, "USB-C", color="m", ha="right" if ux > 0 else "left")
    for xs in bottom_slot_xs():
        a.plot([xs, xs], [0, WALL], c="r", lw=1.5)
    a.add_patch(Circle((CABLE_X_A, 0), CABLE_D / 2, fill=False, ec="b", lw=2, ls="--"))
    a.text(CABLE_X_A, -3, "под провод\n(заужение изнутри)", color="b", ha="center", va="top", fontsize=8)
    a.set_title("%s: вид спереди (крышка снята). Оранжевый — датчик (K22 / S8), зелёный — экран, красный — вентиляция и заужения дна, "
                "синий — провод, фиолетовый — угловые блоки" % PREFIX, fontsize=8.5)
    a.set_xlim(-W / 2 - 6, W / 2 + 6)
    a.set_ylim(-9, H + 4)
    a.set_aspect("equal")

    # ---- разрез (Z-Y), X = 0 ----
    rect(b, 0, BACK_T, 0, H, ec="k", lw=2)
    rect(b, 0, ZL, 0, WALL, ec="k", lw=2)
    rect(b, 0, ZL, H - WALL, H, ec="k", lw=2)
    rect(b, ZL, D, 0, H, ec="gray", lw=2)
    if IS_S8:
        rect(b, S8_Z0, S8_Z0 + S8_H, S8_YC - S8_W / 2, S8_YC + S8_W / 2, ec="darkorange", lw=2)
        rect(b, BACK_T, S8_Z0, S8_YC - S8_GUIDE_YO, S8_YC + S8_GUIDE_YO, ec="darkorange", ls=":")
        for sy in (-1, 1):
            rect(b, BACK_T, S8_Z0 + S8_GUIDE_H, *sorted((S8_YC + sy * S8_GUIDE_YO, S8_YC + sy * (S8_GUIDE_YO + S8_GUIDE_T))),
                 ec="darkorange", lw=1)
        for _, y in s8_pins():
            rect(b, S8_Z0 + S8_H - S8_PIN_PRESS, ZL, y - S8_PIN_D / 2, y + S8_PIN_D / 2, ec="m", lw=1.5)
        b.text((BACK_T + S8_Z0) / 2, S8_YC, "рёбра", color="darkorange", ha="center", va="center", fontsize=7, rotation=90)
        b.text(S8_Z0 + S8_H / 2, S8_YC, "S8", color="darkorange", ha="center", va="center")
        b.plot([S8_Z0 - S8_BOT_SLOT_DOWN, ZL - S8_BOT_SLOT_TOP], [WALL / 2] * 2, c="r", lw=4, alpha=.6)
        b.text(ZL + 1, WALL / 2 - 4, "нижние щели", color="r", ha="center", fontsize=7)
    else:
        z_k = BACK_T + K22_STAND_H
        rect(b, BACK_T, z_k, K22_Y0, K22_Y0 + K22_W, ec="darkorange", ls=":")
        rect(b, z_k, z_k + K22_T, K22_Y0, K22_Y0 + K22_W, ec="darkorange", lw=2)
        rect(b, z_k + K22_T, z_k + K22_T + K22_HEIGHT, K22_Y0 + 6, K22_Y0 + K22_W - 6, ec="darkorange", ls="--")
    if not IS_S8:
        b.text(z_k + K22_HEIGHT / 2 + 2, K22_YC, "K22\nкамера 35", color="darkorange", ha="center", va="center")
    if IS_TD:
        z_pf = ZL - TD_STACK_H
        rect(b, z_pf - TD_PCB_T, z_pf, YD - TD_PCB_W / 2, YD + TD_PCB_W / 2, ec="green", lw=2)
        rect(b, z_pf, ZL, YD - TD_LCD_W / 2, YD + TD_LCD_W / 2, ec="green", ls="--")
        rect(b, z_pf - TD_PCB_T - TD_BACK_H, z_pf - TD_PCB_T, YD - 11.5, YD + 11.5, ec="green", ls=":")
    else:
        rect(b, Z_PCB - PCB_T, Z_PCB, PCB_Y0, PCB_Y0 + PCB_W, ec="green", lw=2)
        rect(b, Z_PCB - PCB_T - BACK_PARTS_H, Z_PCB - PCB_T, PCB_Y0 + 4, PCB_Y0 + PCB_W - 4, ec="green", ls=":")
        rect(b, Z_PCB, Z_PCB + GLASS_H, YD - GLASS_W / 2, YD + GLASS_W / 2, ec="green", ls="--")
    _, _, ny0, ny1 = nrf_rect(0, NRF_L, -NRF_T / 2, NRF_T / 2)
    rect(b, BACK_T, BACK_T + NRF_W, ny0, ny1, ec="teal", lw=2)
    b.text(BACK_T + NRF_W / 2, (ny0 + ny1) / 2 - 3, "nRF24", color="teal", ha="center", va="center", fontsize=8)
    b.add_patch(Circle((BACK_T + CABLE_Z_OFF, 0), CABLE_D / 2, fill=False, ec="b", lw=2, ls="--"))
    uz = usb_center()[1]
    b.plot([uz - USB_H / 2, uz + USB_H / 2], [uy, uy], c="m", lw=6, alpha=.6)
    b.text(uz, uy + 4, "USB-C", color="m", ha="center")
    ysc = (SENS_Y0 + SENS_Y1) / 2
    b.plot([BACK_T + 4, ZL - 4], [ysc, ysc], c="r", lw=5, alpha=.5)
    b.text((BACK_T + ZL) / 2, ysc - 5, "боковые щели (вдоль Z)", color="r", ha="center", fontsize=8)
    b.set_title("Разрез (Z - глубина от стены, Y - вверх); глубина %.1f мм" % D, fontsize=9)
    b.set_xlim(-4, D + 8)
    b.set_ylim(-9, H + 4)
    b.set_aspect("equal")
    for ax in (a, b):
        ax.grid(True, alpha=.25)
    fig.tight_layout()
    fig.savefig(path)
    print("layout:", path)


def preview(body, lid, dm, path):
    """Предпросмотр (matplotlib): корпус с платами без крышки и крышка отдельно."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    except ImportError:
        return
    fig = plt.figure(figsize=(16, 8), dpi=110)
    for i, (title, only_lid) in enumerate((("корпус + платы (без крышки)", False), ("крышка (вид сзади)", True))):
        ax = fig.add_subplot(1, 2, i + 1, projection="3d")
        items = [(lid, (0.85, 0.85, 0.9, 1.0))] if only_lid else [(body, (0.25, 0.5, 0.8, 1.0))] + [
            (dm[k], (0.9, 0.55, 0.1, 0.9) if k in ("k22", "s8") else (0.2, 0.65, 0.3, 0.9)) for k in ("k22", "s8", "pcb", "glass", "nrf24") if k in dm]
        for shape, col in items:
            verts, tris = shape.val().tessellate(0.15, 0.3)
            polys = [[(verts[a].x, verts[a].y, verts[a].z) for a in t] for t in tris]
            ax.add_collection3d(Poly3DCollection(polys, facecolor=col, edgecolor=(0, 0, 0, 0.08), linewidth=0.1))
        ax.set_xlim(-W / 2 - 5, W / 2 + 5); ax.set_ylim(0, H + 5); ax.set_zlim(0, 65)
        ax.set_box_aspect((W + 10, H + 5, 65))
        ax.view_init(24, -62 if not only_lid else 118)
        ax.set_title(title, fontsize=10)
    fig.tight_layout()
    fig.savefig(path)
    print("preview:", path)


# ------------------------------------ таблица параметров для Readme.md ------------------------------------
DERIVED = [("W", "ширина корпуса, мм"), ("H", "высота корпуса, мм"), ("D", "глубина корпуса, мм"),
           ("INNER_W", "внутренняя ширина"), ("INNER_D", "внутренняя глубина (до крышки)"),
           ("ZL", "расстояние от стены до плоскости крышки"), ("SENS_Y0", "низ зоны датчика K22 / S8 (от низа корпуса)"),
           ("YD", "центр платы экрана (от низа корпуса)"), ("WALLHOLE_Y", "высота заужений под шурупы в стену"),
           ("ES_WIN_L", "окно экрана (ES3C28P): длина"), ("ES_WIN_W", "окно экрана (ES3C28P): высота"),
           ("TD_WIN_L", "окно экрана (T-Display-S3): длина"), ("TD_WIN_W", "окно экрана (T-Display-S3): высота")]


def params_markdown():
    """Таблицы параметров из блока PARAMETERS + расчётные размеры обоих корпусов (из Build/enclosure/*_dims.json)."""
    src = open(os.path.abspath(__file__), encoding="utf-8").read()
    start = src.index("# ===================================== PARAMETERS")
    end = src.index("# ======================================================================================\n\n# --- расчёт")
    g = globals()
    out = []
    pat = re.compile(r"^([A-Z][A-Z0-9_]*(?:, [A-Z][A-Z0-9_]*)*) = .*?(?:#\s*(.*))?$")
    for line in src[start:end].splitlines():
        m_sec = re.match(r"^# --- (.+?) ---$", line)
        if m_sec:
            out += ["", "**%s**" % m_sec.group(1), "", "| Параметр | Значение | Описание |", "|---|---|---|"]
            continue
        m = pat.match(line)
        if not m:
            continue
        names = m.group(1).split(", ")
        out.append("| `%s` | %s | %s |" % (", ".join(names), ", ".join(str(g[n]) for n in names), (m.group(2) or "").strip()))
    dims = {}
    for b, pref in BOARDS.items():
        pth = os.path.join(OUT, pref + "_dims.json")
        if os.path.isfile(pth):
            dims[pref] = json.load(open(pth, encoding="utf-8"))
    out += ["", "**Расчётные размеры** (не задаются, считаются из параметров), мм", "",
            "| Величина | " + " | ".join(dims) + " | Что |", "|---|" + "---|" * len(dims) + "---|"]
    for n, t in DERIVED:
        out.append("| `%s` | " % n + " | ".join("%.1f" % d[n] if n in d else "-" for d in dims.values()) + " | %s |" % t)
    return "\n".join(out).lstrip("\n") + "\n"


def update_project():
    """Заменяет содержимое подраздела «### 13.7 Параметры» в Readme.md (до следующего заголовка или конца файла)."""
    title = "### 13.7 Параметры"
    text = open(README_MD, encoding="utf-8").read()
    mt = re.search(r"^" + re.escape(title) + r"[ \t]*$", text, re.M)       # только строка-заголовок, не упоминание в тексте
    if mt is None:
        print("Readme.md: заголовок «%s» не найден" % title)
        return False
    i, j = mt.start(), mt.end()
    m = re.search(r"^#{1,3} ", text[j:], re.M)       # следующий заголовок
    tail = ("\n" + text[j + m.start():]) if m else ""
    body = ("%s\n\n_Таблица сформирована автоматически: `python Scripts/enclosure.py --update-project` "
            "(источник — блоки PARAMETERS в Scripts/enclosure.py; правьте там)._\n\n%s" % (title, params_markdown()))
    open(README_MD, "w", encoding="utf-8", newline="\n").write(text[:i] + body + tail)
    print("Readme.md: таблица параметров корпуса обновлена")
    return True


# ------------------------------------ основной сценарий ------------------------------------
def build_one():
    os.makedirs(OUT, exist_ok=True)
    body, dm = build_body(), dummies()
    lids = {"lid_wall": build_lid(True), "lid_table": build_lid(False)}
    clips = td_clips_in_lid() if IS_TD and not IS_S8 else []
    ears = td_ears_in_place() if IS_S8 else []
    pins = td_buttons_in_lid() if IS_TD else []

    print("[%s] корпус %.1f x %.1f x %.1f мм, внутри %.1f x %.1f x %.1f"
          % (PREFIX, W, H, D, INNER_W, H - 2 * WALL, INNER_D))
    ok = True
    parts = {"body": body, **lids}
    for name, part in parts.items():
        valid = part.val().isValid()
        n = len(part.solids().vals())
        print("  %-9s solids=%d valid=%s объём=%.1f см3" % (name, n, valid, part.val().Volume() / 1000))
        ok &= valid and n == 1

    gap_k22 = WALLHOLE_Y - WALLHOLE_D / 2 - SENS_Y1
    # до держателей и зоны проводов nRF24 (по плану; выемка — в дне, провода — выше, но нужен доступ отвёрткой)
    zones = [nrf_rect(0, NRF_L + 0.3 + NRF_STOP_T, -NRF_HOLD_N, NRF_HOLD_N),
             nrf_rect(NRF_L - NRF_HEADER_L, NRF_L, NRF_T / 2, NRF_T / 2 + NRF_WIRES_L)]
    gap_nrf = min(rect_dist(sx * WALLHOLE_X_A, WALLHOLE_Y, z) - WALLHOLE_D / 2 for sx in (-1, 1) for z in zones)
    print("  шурупы в стену: Y=%.1f, зазор выемки до датчика (K22 / S8) %.1f мм, до nRF24 (держатели и провода) %.1f мм" % (WALLHOLE_Y, gap_k22, gap_nrf))
    ok &= gap_k22 >= 0 and gap_nrf >= 0
    print("  пересечения макетов и деталей (мм3, должно быть 0):")
    press = len(S8_PIN_UV) * math.pi * (S8_PIN_D / 2) ** 2 * S8_PIN_PRESS     # S8: натяг штырьков крышки — норма
    for k, d in dm.items():
        for pn, part in parts.items():
            v = volume_of(part, d) - (press if k == "s8" and pn.startswith("lid") else 0)
            ok &= v < 1.0
            if v >= 1.0:
                print("    %-10s x %-9s %8.2f  <-- ПЕРЕСЕЧЕНИЕ" % (k, pn, v))
    for ln, lid in lids.items():
        v = volume_of(body, lid)
        ok &= v < 1.0
        print("    body x %-9s %8.2f%s" % (ln, v, "" if v < 1.0 else "  <-- ПЕРЕСЕЧЕНИЕ"))
    for i, c in enumerate(clips):                 # защёлки: без пересечений с крышкой, корпусом и платой
        for nm, other in [("lid_table", lids["lid_table"]), ("body", body)] + [(k, d) for k, d in dm.items() if k != "k22"]:
            v = volume_of(c, other)
            ok &= v < 0.05
            if v >= 0.05:
                print("    clip%d x %-9s %8.3f  <-- ПЕРЕСЕЧЕНИЕ" % (i + 1, nm, v))
    if clips:
        ok &= all(c.val().isValid() for c in clips)
        print("    защёлки: %d шт, без пересечений" % len(clips) if ok else "    защёлки: ЕСТЬ ПРОБЛЕМЫ")
    pins_ok = True
    for i, p in enumerate(pins):                  # штырьки кнопок: без пересечений с крышками и платой
        for nm, other in [("lid_wall", lids["lid_wall"]), ("lid_table", lids["lid_table"])] + \
                         [(k, d) for k, d in dm.items() if k != "k22"]:
            v = volume_of(p, other)
            if v >= 0.01:
                pins_ok = False
                print("    pin%d x %-9s %8.3f  <-- ПЕРЕСЕЧЕНИЕ" % (i + 1, nm, v))
    if pins:
        pins_ok &= all(p.val().isValid() for p in pins)
        ok &= pins_ok
        print("    штырьки кнопок: %d шт, %s" % (len(pins), "без пересечений" if pins_ok else "ЕСТЬ ПРОБЛЕМЫ"))
    ears_ok = True
    for i, e in enumerate(ears):                  # уши: без пересечений с корпусом, крышками, платой, датчиком
        for nm, other in [("body", body), ("lid_wall", lids["lid_wall"]), ("lid_table", lids["lid_table"])] + \
                         list(dm.items()):
            v = volume_of(e, other)
            if v >= 0.01:
                ears_ok = False
                print("    ear%d x %-9s %8.3f  <-- ПЕРЕСЕЧЕНИЕ" % (i + 1, nm, v))
    if ears:
        ears_ok &= all(e.val().isValid() for e in ears)
        ok &= ears_ok
        print("    уши платы: %d шт, %s" % (len(ears), "без пересечений" if ears_ok else "ЕСТЬ ПРОБЛЕМЫ"))
    if IS_S8:   # вентиляция датчика (монтаж на стену): площади щелей и пути диффузии до окна
        n_b = len(bottom_slot_xs())
        a_bot = n_b * SLOT_W * (ZL - S8_BOT_SLOT_TOP - S8_Z0 + S8_BOT_SLOT_DOWN)
        win = s8_window(ZL, D).val()
        lv = 0.0
        for x in lidvent_xs():
            lv += win.intersect(box(x - SLOT_W / 2, x + SLOT_W / 2, 0, H, ZL, D).val()).Volume() / LID_T
        print("  S8: низ датчика Z=%.1f, рёбра %.1f мм; нижние щели %d x %.1f мм = %.0f мм2 (Z %.1f..%.1f), "
              "щели крышки над окном %.0f мм2; путь до окна: через крышку %.1f мм, от нижних щелей %.1f мм"
              % (S8_Z0, S8_Z0 - BACK_T, n_b, ZL - S8_BOT_SLOT_TOP - S8_Z0 + S8_BOT_SLOT_DOWN, a_bot,
                 S8_Z0 - S8_BOT_SLOT_DOWN, ZL - S8_BOT_SLOT_TOP, lv, LID_T + S8_TOP_GAP,
                 min(s8_xy(u, v)[1] for u, v in S8_WIN_UV) - WALL))

    # STL (крышки перевёрнуты лицевой стороной вниз), имена по плате; масштаб — компенсация усадки (for_print)
    print("  STL с компенсацией усадки: XY +%.2f %%, Z +%.2f %%" % (SHRINK_XY * 100, SHRINK_Z * 100))
    bp = for_print(body)
    cq.exporters.export(bp, os.path.join(OUT, PREFIX + "_body.stl"), tolerance=0.05, angularTolerance=0.1)
    for ln, lid in lids.items():
        lp = for_print(lid.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, H, D)))
        cq.exporters.export(lp, os.path.join(OUT, "%s_%s.stl" % (PREFIX, ln)), tolerance=0.05, angularTolerance=0.1)
        bb = lp.val().BoundingBox()
        print("  %s_%s.stl  %.1f x %.1f x %.1f мм (для печати)" % (PREFIX, ln, bb.xlen, bb.ylen, bb.zlen))
    bb = bp.val().BoundingBox()
    print("  %s_body.stl  %.1f x %.1f x %.1f мм" % (PREFIX, bb.xlen, bb.ylen, bb.zlen))
    if IS_TD and not IS_S8:
        cp = for_print(td_clips_print())
        cq.exporters.export(cp, os.path.join(OUT, PREFIX + "_clips_x4.stl"), tolerance=0.02, angularTolerance=0.1)
        bb = cp.val().BoundingBox()
        print("  %s_clips_x4.stl  %.1f x %.1f x %.1f мм (4 защёлки, печать плашмя)" % (PREFIX, bb.xlen, bb.ylen, bb.zlen))
    if IS_S8:
        ep = for_print(ears_print())
        cq.exporters.export(ep, os.path.join(OUT, PREFIX + "_ears_x4.stl"), tolerance=0.02, angularTolerance=0.1)
        bb = ep.val().BoundingBox()
        print("  %s_ears_x4.stl  %.1f x %.1f x %.1f мм (4 уха платы, печать плашмя)" % (PREFIX, bb.xlen, bb.ylen, bb.zlen))
    if IS_TD:
        bp2 = for_print(td_buttons_print())
        cq.exporters.export(bp2, os.path.join(OUT, PREFIX + "_buttons_x2.stl"), tolerance=0.02, angularTolerance=0.1)
        bb = bp2.val().BoundingBox()
        print("  %s_buttons_x2.stl  %.1f x %.1f x %.1f мм (2 штырька кнопок, печать наконечником вниз)" % (PREFIX, bb.xlen, bb.ylen, bb.zlen))

    asm = (cq.Assembly(name=PREFIX + "_enclosure")
           .add(body, name="body", color=cq.Color(0.2, 0.5, 0.8, 1))
           .add(lids["lid_table"], name="lid", color=cq.Color(0.9, 0.9, 0.9, 0.6)))
    for k, d in dm.items():
        asm.add(d, name=k, color=cq.Color(0.9, 0.6, 0.1, 0.5) if k in ("k22", "s8") else cq.Color(0.1, 0.6, 0.2, 0.5))
    for i, c in enumerate(clips):
        asm.add(c, name="clip%d" % (i + 1), color=cq.Color(0.8, 0.2, 0.8, 1))
    for i, p in enumerate(pins):
        asm.add(p, name="button_pin%d" % (i + 1), color=cq.Color(0.9, 0.1, 0.1, 1))
    for i, e in enumerate(ears):
        asm.add(e, name="ear%d" % (i + 1), color=cq.Color(0.8, 0.2, 0.8, 1))
    asm.export(os.path.join(OUT, PREFIX + "_assembly.step"))
    layout(os.path.join(OUT, PREFIX + "_layout.png"))
    preview(body, lids["lid_wall"], dm, os.path.join(OUT, PREFIX + "_preview.png"))
    json.dump({n: globals()[n] for n, _ in DERIVED}, open(os.path.join(OUT, PREFIX + "_dims.json"), "w", encoding="utf-8"))
    print("  OK" if ok else "  ЕСТЬ ПРОБЛЕМЫ")
    return ok


def main():
    if "--board" not in " ".join(sys.argv) and not any(a.startswith("--board=") for a in sys.argv):
        ok = True                                         # без --board: обе платы (каждая в отдельном процессе)
        for b in BOARDS:
            r = subprocess.run([sys.executable, os.path.abspath(__file__), "--board", b])
            ok &= r.returncode == 0
        if "--update-project" in sys.argv:
            update_project()
        return 0 if ok else 1
    ok = build_one()
    if "--update-project" in sys.argv:
        update_project()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
