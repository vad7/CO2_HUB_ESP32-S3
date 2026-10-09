// sys_info.h - состояние системы: задачи FreeRTOS, загрузка ядер процессора и температура кристалла ESP32-S3.
// Источник - встроенная статистика FreeRTOS (Arduino-ESP32 3.x: CONFIG_FREERTOS_USE_TRACE_FACILITY,
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, счётчик времени задачи - esp_timer, мкс, 32 бита).
// Раз в SYSINFO_PERIOD_MS update() снимает список задач (uxTaskGetSystemState):
//   - время задачи накапливается в 64 бита по приращениям 32-битного счётчика (он переполняется раз в ~71,6 мин;
//     приращение за период вычитанием без знака верно, пока период < 71 мин);
//   - «Итого» - доля задачи от суммы времени всех задач (оба ядра) с запуска, 0,1 %;
//   - загрузка ядра - 100 % − доля его задачи простоя (IDLE0 / IDLE1) за последний период.
// Мин. свободный стек (usStackHighWaterMark) - в БАЙТАХ: в ESP-IDF StackType_t = uint8_t.
// Температура кристалла - встроенный датчик (temperatureRead()), это температура чипа, а не воздуха.
// Потоки: update() и чтение (task(), cpuLoad() …) - под CfgLock (loop() и веб).
#pragma once
#include <stdint.h>
#include "config.h"

namespace SysInfo {
    enum TaskState : uint8_t { TASK_RUNNING, TASK_READY, TASK_BLOCKED, TASK_SUSPENDED, TASK_DELETED, TASK_STATE_COUNT };

    struct TaskRow {
        char     name[SYSINFO_TASK_NAME_LEN];   // имя задачи (обрезано до configMAX_TASK_NAME_LEN)
        uint8_t  state;                         // TaskState
        uint8_t  prio;                          // текущий приоритет
        int8_t   core;                          // ядро, к которому привязана; -1 - любое
        uint32_t stackMinB;                     // мин. свободный стек с момента создания, байт
        uint64_t timeUs;                        // время работы с запуска (с первого снимка), мкс
        uint16_t shareTenths;                   // «Итого»: доля времени ЦП с запуска, 0,1 %
    };

    void        begin();
    void        update(uint32_t now);           // из loop() под CfgLock: снимок раз в SYSINFO_PERIOD_MS
    uint8_t     cpuLoad(uint8_t core);          // загрузка ядра 0 / 1 за последний период, %
    uint8_t     cores();                        // число ядер
    bool        chipTempValid();
    int16_t     chipTempTenths();               // температура кристалла, 0,1 °C
    uint32_t    snapshotId();                   // растёт с каждым снимком - признак «пора перерисовать»
    uint8_t     taskCount();                    // задач в последнем снимке (по убыванию приоритета, затем по имени)
    bool        taskOverflow();                 // задач больше SYSINFO_TASKS_MAX - показаны не все
    const TaskRow* task(uint8_t i);             // nullptr - нет такой
    const char* stateName(uint8_t state);       // «Выполняется», «Готова», …
    const char* stateShort(uint8_t state);      // 1 буква для экрана: W выполняется, R готова, B ждёт, S приост., D удалена
}
