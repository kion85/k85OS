#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Command-line editor for terminals that deliver raw bytes (SSH): cursor keys, history
// (Up/Down, 20 entries kept between sessions), Tab completion of commands and files,
// Home/End/Delete, Ctrl-A/E/U/K/W/C/D/L.

#define K85_LE_MAX  160
#define K85_LE_HIST 20

enum K85LeAction {
    K85_LE_NONE = 0,   // keep reading
    K85_LE_LINE,       // a full line is ready in ed->buf
    K85_LE_EOF,        // Ctrl-D on an empty line
    K85_LE_CANCEL,     // Ctrl-C: line dropped, prompt already printed
};

struct K85LineEditor {
    char buf[K85_LE_MAX];
    char saved[K85_LE_MAX];     // line being typed when history browsing started
    int len, cur;
    int esc_state;              // 0 normal, 1 after ESC, 2 inside ESC [ ..., 3 after ESC O
    int csi_arg;
    int hist_pos;               // -1 = editing a fresh line
    bool last_was_cr;
    bool tab_pending;           // previous key was Tab: a second Tab lists the candidates
    const char *prompt;
};

void k85_le_init(struct K85LineEditor *e, const char *prompt);
void k85_le_reset(struct K85LineEditor *e);       // after a line was executed (prompt is printed by the caller)

// Feeds one byte from the terminal. Everything that must be sent back (echo, cursor moves,
// completion lists) is written to out as a NUL-terminated string (possibly empty).
enum K85LeAction k85_le_feed(struct K85LineEditor *e, uint8_t c, char *out, size_t out_size);

void k85_le_history_add(const char *line);        // ignores empty lines, duplicates and "ftppass ..." (passwords)
int k85_le_history_count(void);
const char *k85_le_history_get(int idx_from_newest);   // 0 = newest, NULL if out of range
