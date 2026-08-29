#include "task_manager.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "power.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define K85_TM_MAX_TASKS 24
#define K85_TM_VISIBLE_ROWS 8

static const char *state_letter(eTaskState st) {
    switch (st) {
        case eRunning:   return "R";
        case eReady:     return "Y";
        case eBlocked:   return "B";
        case eSuspended: return "S";
        case eDeleted:   return "D";
        default:         return "?";
    }
}

static void tm_draw(TaskStatus_t *tasks, UBaseType_t count, int offset) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextWrap(false);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 2);
    M5.Display.printf("Tasks: %u", (unsigned)count);

    M5.Display.setTextColor(0x888888, bg);
    M5.Display.setCursor(4, 14);
    M5.Display.print("NAME            ST PRI STACK");

    int y = 26;
    int end = offset + K85_TM_VISIBLE_ROWS;
    if (end > (int)count) end = (int)count;

    for (int i = offset; i < end; i++) {
        TaskStatus_t *t = &tasks[i];
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, y);

        char name[16];
        snprintf(name, sizeof(name), "%-15.15s", t->pcTaskName);

        // stack watermark даётся в словах (StackType_t == uint8_t на этой
        // платформе, см. разбор SSH-стека выше по проекту) — то есть уже в
        // байтах напрямую.
        M5.Display.printf("%s%s  %2u %4uB", name,
                           state_letter(t->eCurrentState),
                           (unsigned)t->uxCurrentPriority,
                           (unsigned)t->usStackHighWaterMark);
        y += 12;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (offset > 0) {
        M5.Display.setCursor(230, 14);
        M5.Display.print("^");
    }
    if (end < (int)count) {
        M5.Display.setCursor(230, 26 + (K85_TM_VISIBLE_ROWS - 1) * 12);
        M5.Display.print("v");
    }
    M5.Display.setCursor(4, M5.Display.height() - 10);
    M5.Display.print("A=scroll B=refresh A+B=exit");
}

void k85_run_task_manager(void) {
    static TaskStatus_t tasks[K85_TM_MAX_TASKS];
    int offset = 0;

    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count > K85_TM_MAX_TASKS) count = K85_TM_MAX_TASKS;
    uint32_t total_runtime;
    count = uxTaskGetSystemState(tasks, count, &total_runtime);

    tm_draw(tasks, count, offset);

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            if (offset + K85_TM_VISIBLE_ROWS < (int)count) {
                offset += K85_TM_VISIBLE_ROWS;
            } else {
                offset = 0;
            }
            tm_draw(tasks, count, offset);
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            UBaseType_t n = uxTaskGetNumberOfTasks();
            if (n > K85_TM_MAX_TASKS) n = K85_TM_MAX_TASKS;
            count = uxTaskGetSystemState(tasks, n, &total_runtime);
            offset = 0;
            tm_draw(tasks, count, offset);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}