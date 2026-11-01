#pragma once
// K85FX1: lock screen scene engine
#include <stdint.h>
void k85_lockfx_begin(void);                 // call when the lock screen starts
void k85_lockfx_end(void);                   // call when it ends (frees the frame buffers)
void k85_lockfx_button(void);                // a button was tapped: may trigger an explosion
void k85_lockfx_frame(const char *time_str, const char *date_str, const char *name, const char *info);
void k85_lockfx_settings_menu(void);         // Settings -> Lock screen
// K85FX2: battery
uint32_t k85_lockfx_interval(uint32_t idle_ms);   // how often to draw a frame after idle_ms without input
bool k85_lockfx_busy(void);                      // a burst is playing: use the fast rate
int k85_lockfx_power(void);                      // 0 Saver, 1 Balanced, 2 Smooth
