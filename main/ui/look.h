#pragma once
#include <cstdint>
#include "M5Unified.h"

// K85LOOK1: user wallpapers (images from /littlefs/wall) and the status bar banner
enum K85WallSlot { K85_WALL_LOCK = 0, K85_WALL_MAIN = 1, K85_WALL_SYS = 2 };

bool k85_wall_active(K85WallSlot slot);                          // a wallpaper is set and usable
void k85_wall_draw(K85WallSlot slot, uint32_t fallback_bg);      // wallpaper, or a plain fill when none
void k85_wall_text(uint32_t fg, uint32_t bg);                    // transparent text over the system wallpaper
bool k85_banner_active(void);
bool k85_banner_draw(int x0, int wpart);                         // paints the banner part x0..x0+wpart of the status strip
void k85_look_run_settings(void);                                // Settings -> Look
// K85LOOK2
void k85_wall_text_main(uint32_t fg, uint32_t bg);                        // transparent text over the main menu wallpaper
bool k85_wall_push(K85WallSlot slot, lgfx::LovyanGFX *dst);               // copies a ready wallpaper into a canvas
bool k85_image_push(const char *name, int dim, lgfx::LovyanGFX *dst);     // same for any image of /wall (lock scene background)
void k85_look_wall_menu(K85WallSlot slot);                                // choose file / dim / remove for one slot
