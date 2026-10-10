#pragma once

#include <stdbool.h>
#include <stddef.h>

enum K85ShotResult {
    K85_SHOT_OK,
    K85_SHOT_NO_SPACE,
    K85_SHOT_NO_MEM,
    K85_SHOT_IO,
    K85_SHOT_UNREADABLE,   // this display cannot be read back
    K85_SHOT_BUSY,
    K85_SHOT_TOO_MANY,
};

// Saves the current screen as a 24-bit BMP in /littlefs/shots/shot_NNN.bmp.
// path_out (optional) receives the file path on success.
K85ShotResult k85_screenshot_take(char *path_out, size_t path_size);
const char *k85_screenshot_result_text(K85ShotResult r);

// Starts the background watcher: a short press of A+B together (released within ~0.4 s)
// takes a screenshot. Holding A+B (the exit gesture) is not affected. Call once.
void k85_screenshot_start(void);

// Gesture on/off (stored in k85_svc.cfg, key "screenshot", default on).
bool k85_screenshot_gesture_enabled(void);
void k85_screenshot_set_gesture(bool on);
