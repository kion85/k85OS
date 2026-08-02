#include "melodies.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "common.h"
#include "sound.h"
#include "list_menu.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

struct Note { int freq; int dur_ms; };

// Те же ноты, что и BUILTIN_MELODIES в оригинале
static const Note MELODY_MARIO[] = {
    {660,100},{660,100},{0,100},{660,100},
    {0,100},{520,100},{660,100},{0,100},
    {784,100},{0,200},{392,200},
};
static const Note MELODY_NOKIA[] = {
    {523,200},{523,200},{0,200},{523,200},
    {0,200},{392,200},{523,200},{0,200},
    {659,400},{0,200},
};
static const Note MELODY_STARWARS[] = {
    {392,200},{392,200},{392,200},{0,100},
    {311,100},{523,200},{392,200},{0,100},
    {311,100},{523,200},{392,200},{0,100},
    {330,100},{392,100},{0,100},{311,100},
    {523,200},{392,200},
};
static const Note MELODY_TETRIS[] = {
    {330,150},{349,150},{392,150},{349,150},
    {392,150},{440,150},{330,150},{294,150},
    {262,150},{0,150},
};

struct MelodyEntry { const char *name; const Note *notes; int count; };
static const MelodyEntry BUILTIN_MELODIES[] = {
    {"Mario", MELODY_MARIO, (int)(sizeof(MELODY_MARIO)/sizeof(Note))},
    {"Nokia", MELODY_NOKIA, (int)(sizeof(MELODY_NOKIA)/sizeof(Note))},
    {"Star Wars", MELODY_STARWARS, (int)(sizeof(MELODY_STARWARS)/sizeof(Note))},
    {"Tetris", MELODY_TETRIS, (int)(sizeof(MELODY_TETRIS)/sizeof(Note))},
};
#define BUILTIN_MELODIES_COUNT (int)(sizeof(BUILTIN_MELODIES)/sizeof(BUILTIN_MELODIES[0]))

// Возвращает true если проиграно до конца, false если прервано A+B
static bool play_melody(const Note *notes, int count) {
    for (int i = 0; i < count; i++) {
        k85_input_update();
        if (k85_ab_held(400)) {
            k85_speaker_stop();
            k85_wait_ab_release();
            return false;
        }
        if (notes[i].freq > 0) {
            k85_play_tone(notes[i].freq, notes[i].dur_ms);
            vTaskDelay(pdMS_TO_TICKS(notes[i].dur_ms));
            k85_speaker_stop();
        } else {
            vTaskDelay(pdMS_TO_TICKS(notes[i].dur_ms));
        }
    }
    return true;
}

static void run_tone_composer(void) {
    static const int FREQ_OPTS[] = {0, 262, 294, 330, 349, 392, 440, 523, 659, 784};
    const int FREQ_OPTS_COUNT = (int)(sizeof(FREQ_OPTS)/sizeof(FREQ_OPTS[0]));

    Note seq[20];
    int seq_len = 0;
    int freq_sel = 0;

    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.print("Tone Composer");
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 14);
        M5.Display.print("A=scroll B=pick");
        M5.Display.setCursor(4, 26);
        M5.Display.print("A+B=play+exit");
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0xFFFF00, bg);
        M5.Display.fillRect(4, 38, W - 8, 18, 0x111111);
        M5.Display.setCursor(8, 40);
        M5.Display.printf("> %dHz", FREQ_OPTS[freq_sel]);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 60);
        if (seq_len == 0) {
            M5.Display.print("Seq:(empty)");
        } else {
            char buf[80] = "Seq:";
            int start = seq_len > 8 ? seq_len - 8 : 0;
            for (int i = start; i < seq_len; i++) {
                char part[12];
                snprintf(part, sizeof(part), " %d", seq[i].freq);
                strncat(buf, part, sizeof(buf) - strlen(buf) - 1);
            }
            M5.Display.print(buf);
        }
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.printf("%d/20 notes", seq_len);
        k85_draw_battery_icon();
    };

    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            if (seq_len > 0) {
                k85_show_message("Playing sequence...");
                play_melody(seq, seq_len);
            }
            return;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            freq_sel = (freq_sel + 1) % FREQ_OPTS_COUNT;
            if (FREQ_OPTS[freq_sel] > 0) {
                k85_play_tone(FREQ_OPTS[freq_sel], 120);
                vTaskDelay(pdMS_TO_TICKS(120));
                k85_speaker_stop();
            }
            draw();
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            M5.Display.fillRect(4, 38, W - 8, 18, bg);
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(0xFFFF00, bg);
            M5.Display.setCursor(8, 42);
            M5.Display.print("Dur: A=short B=long");

            bool dur_picked = false;
            int dur_val = 100;
            while (!dur_picked) {
                k85_input_update();
                if (k85_ab_held(500)) {
                    k85_wait_ab_release();
                    draw();
                    dur_picked = false;
                    break;
                }
                if (k85_btn_a_pressed()) {
                    k85_wake_screen();
                    dur_val = 100;
                    dur_picked = true;
                }
                if (k85_btn_b_pressed()) {
                    k85_wake_screen();
                    dur_val = 300;
                    dur_picked = true;
                }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            if (dur_picked) {
                if (seq_len < 20) {
                    seq[seq_len].freq = FREQ_OPTS[freq_sel];
                    seq[seq_len].dur_ms = dur_val;
                    seq_len++;
                }
                if (seq_len >= 20) {
                    k85_show_message("Max 20 notes!\nPlaying...");
                    play_melody(seq, seq_len);
                    return;
                }
                draw();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_melody_player(void) {
    const char *items[BUILTIN_MELODIES_COUNT + 2];
    for (int i = 0; i < BUILTIN_MELODIES_COUNT; i++) items[i] = BUILTIN_MELODIES[i].name;
    items[BUILTIN_MELODIES_COUNT] = "Tone Composer";
    items[BUILTIN_MELODIES_COUNT + 1] = "Back";

    while (true) {
        int idx = k85_run_list_menu("MELODIES", items, BUILTIN_MELODIES_COUNT + 2, nullptr);
        if (idx < 0 || idx == BUILTIN_MELODIES_COUNT + 1) return;
        if (idx == BUILTIN_MELODIES_COUNT) {
            run_tone_composer();
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "Playing:\n%s\nA+B=stop", BUILTIN_MELODIES[idx].name);
            k85_show_message(buf);
            play_melody(BUILTIN_MELODIES[idx].notes, BUILTIN_MELODIES[idx].count);
        }
    }
}
