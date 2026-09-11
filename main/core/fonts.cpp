#include "fonts.h"
#include "M5Unified.h"

const char *k85_font_names[K85_FONT_COUNT] = {
    "Classic", "Font2", "Mono", "Sans", "Serif"
};

void k85_apply_font(int idx) {
    switch (idx) {
        case 0: M5.Display.setFont(&fonts::Font0); break;
        case 1: M5.Display.setFont(&fonts::Font2); break;
        case 2: M5.Display.setFont(&fonts::FreeMono9pt7b); break;
        case 3: M5.Display.setFont(&fonts::FreeSans9pt7b); break;
        case 4: M5.Display.setFont(&fonts::FreeSerif9pt7b); break;
        default: M5.Display.setFont(&fonts::Font0); break;
    }
}
