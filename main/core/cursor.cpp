#include "cursor.h"
#include "config.h"

#include "M5Unified.h"

static float s_cx = 0.0f;
static float s_cy = 0.0f;
static bool s_inited = false;

bool k85_cursor_active(void) {
    // Курсор активен либо явно (Settings -> Cursor: IMU), либо всегда,
    // когда включён Desktop/OS режим (там курсор "по умолчанию").
    return g_config.cursor_mode == 3 || g_config.desktop_mode;
}

void k85_cursor_reset(void) {
    s_cx = (float)(M5.Display.width() / 2);
    s_cy = (float)(M5.Display.height() / 2);
    s_inited = true;
}

void k85_cursor_update(void) {
    if (!s_inited) k85_cursor_reset();

    float ax = 0, ay = 0, az = 0;
    M5.Imu.getAccel(&ax, &ay, &az);

    int W = M5.Display.width();
    int H = M5.Display.height();

    s_cx -= ax * 6.0f;
    s_cy += ay * 6.0f;
    if (s_cx < 2.0f) s_cx = 2.0f;
    if (s_cx > (float)(W - 2)) s_cx = (float)(W - 2);
    if (s_cy < 2.0f) s_cy = 2.0f;
    if (s_cy > (float)(H - 2)) s_cy = (float)(H - 2);
}

int k85_cursor_x(void) { return (int)s_cx; }
int k85_cursor_y(void) { return (int)s_cy; }

void k85_cursor_draw(void) {
    int x = (int)s_cx;
    int y = (int)s_cy;
    uint32_t col = 0x00FFFF;
    M5.Display.drawLine(x - 6, y, x + 6, y, col);
    M5.Display.drawLine(x, y - 6, x, y + 6, col);
    M5.Display.drawCircle(x, y, 8, col);
}
