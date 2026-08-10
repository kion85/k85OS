#include "status_bar_settings.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "../core/config.h"
#include "../core/status_bar.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

#define K85_SB_ITEM_COUNT 12
#define K85_SB_VISIBLE_ROWS 8

struct SbItem {
    const char *label;
    uint32_t bit; // 0 = СЃРїРµС†-РїСѓРЅРєС‚ (С†РІРµС‚ С„РѕРЅР°)
};

static const SbItem k85_sb_items[K85_SB_ITEM_COUNT] = {
    {"Battery %",       SB_BIT_BATTERY_PCT},
    {"Battery: bolt icon", SB_BIT_BATTERY_BOLT},
    {"Time",            SB_BIT_TIME},
    {"Date",            SB_BIT_DATE},
    {"Uptime",          SB_BIT_UPTIME},
    {"WiFi status",     SB_BIT_WIFI},
    {"Bluetooth status",SB_BIT_BLUETOOTH},
    {"Sound volume",    SB_BIT_SOUND},
    {"Steps",           SB_BIT_STEPS},
    {"Free RAM %",      SB_BIT_RAM},
    {"Chip temp",       SB_BIT_TEMP},
    {"Background color",0},
};

static const uint32_t k85_sb_bg_presets[] = {
    0xFFFFFFFF, 0x000000, 0x1A1A2E, 0x330000, 0x003300, 0xFFFFFF,
};
#define K85_SB_BG_PRESET_COUNT (int)(sizeof(k85_sb_bg_presets) / sizeof(k85_sb_bg_presets[0]))

static int s_selected = 0;
static int s_scroll_top = 0;

static void clamp_scroll(void) {
    if (s_selected < s_scroll_top) s_scroll_top = s_selected;
    if (s_selected >= s_scroll_top + K85_SB_VISIBLE_ROWS) s_scroll_top = s_selected - K85_SB_VISIBLE_ROWS + 1;
    if (s_scroll_top < 0) s_scroll_top = 0;
    int max_top = K85_SB_ITEM_COUNT - K85_SB_VISIBLE_ROWS;
    if (max_top < 0) max_top = 0;
    if (s_scroll_top > max_top) s_scroll_top = max_top;
}

static void draw(void) {
    clamp_scroll();

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(fg, bg);
    M5.Display.print("Status bar items");

    int y = 16;
    int last_visible = s_scroll_top + K85_SB_VISIBLE_ROWS;
    if (last_visible > K85_SB_ITEM_COUNT) last_visible = K85_SB_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_sb_items[i].label);

        char val[16] = "";
        if (k85_sb_items[i].bit != 0) {
            snprintf(val, sizeof(val), "%s", (g_config.sb_flags & k85_sb_items[i].bit) ? "ON" : "off");
        } else {
            int idx = 0;
            for (int p = 0; p < K85_SB_BG_PRESET_COUNT; p++) {
                if (k85_sb_bg_presets[p] == g_config.sb_bg_color) { idx = p; break; }
            }
            snprintf(val, sizeof(val), "%s", idx == 0 ? "theme" : "custom");
        }
        M5.Display.setCursor(170, y);
        M5.Display.print(val);
        y += 12;
    }

    if (K85_SB_ITEM_COUNT > K85_SB_VISIBLE_ROWS) {
        if (s_scroll_top > 0) {
            M5.Display.setCursor(225, 16);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("^");
        }
        if (last_visible < K85_SB_ITEM_COUNT) {
            M5.Display.setCursor(225, 16 + (K85_SB_VISIBLE_ROWS - 1) * 12);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("v");
        }
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 4);
    M5.Display.print("A=next B=toggle A+B=back");
}

void k85_run_status_bar_settings(void) {
    s_selected = 0;
    s_scroll_top = 0;
    draw();

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            k85_config_save();
            return;
        }
        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_SB_ITEM_COUNT;
            draw();
        }
        if (k85_btn_b_pressed() && !k85_btn_a_is_down()) {
            uint32_t bit = k85_sb_items[s_selected].bit;
            if (bit != 0) {
                g_config.sb_flags ^= bit;
            } else {
                int cur_idx = 0;
                for (int p = 0; p < K85_SB_BG_PRESET_COUNT; p++) {
                    if (k85_sb_bg_presets[p] == g_config.sb_bg_color) { cur_idx = p; break; }
                }
                cur_idx = (cur_idx + 1) % K85_SB_BG_PRESET_COUNT;
                g_config.sb_bg_color = k85_sb_bg_presets[cur_idx];
            }
            k85_config_save();
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
