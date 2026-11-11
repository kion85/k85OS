// K85BL1: bootloader hook. Hold button A at power-on to go back to the k85OS slot,
// whatever third-party firmware the boot selection points to.
// K85BL10: sixth entry "Custom colors": background / text / accent are chosen right here (saved by the bootloader itself
// in the spare sector 0x13000, no help from k85OS needed).
// K85BL8: the M5PM1 stretches the I2C clock - the bit-banged master now waits for SCL; register writes are verified; with A held
// the menu times out after 10 s (a dark screen must not trap the boot: B or the timeout starts k85OS).
// K85BL7: the menu is shown on EVERY start (also after esp_restart()); a stuck I2C bus is cleared before talking to the PM1.
// K85BL6: the RTC watchdog of the bootloader (~9 s) is fed while the menu waits, otherwise it resets the chip; crashes /
// watchdog resets show the menu too (only esp_restart() skips it). K85BL5: the menu is shown on every start except the app restarting itself / crashing (a battery power-on is not a
// "power-on reset": k85OS powers off with deep sleep). K85BL4: the LCD rail (L3B) is switched on through the M5PM1 power IC first (PYG2 over I2C), otherwise the
// screen stays black. K85BL3: the hook is now a real boot loader menu (GRUB analog). On every power-on (and when A is
// held) it draws the capybara and the menu on the StickS3 LCD, BEFORE any application starts:
//   k85OS / k85OS BIOS / Test Mode / Alt Firmware / Rec (flash a .bin from the SD card)
// A = next, B = select, auto-boot of the highlighted entry after 3 s. A held at power-on = no timeout.
// The choice is passed to k85OS through a one-shot record in the spare flash sector 0x12000.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#ifndef K85_BL_SIM
#include "hal/wdt_hal.h"
#endif
#include "soc/gpio_sig_map.h"
#if __has_include("soc/reset_reasons.h")
#include "soc/reset_reasons.h"
#endif
#include "bootloader_flash_priv.h"
#include "bootloader_common.h"
#include "esp_flash_partitions.h"
#include "soc/rtc_cntl_reg.h"

#ifndef RESET_REASON_CHIP_POWER_ON
#define RESET_REASON_CHIP_POWER_ON 0x01
#endif

static const char *TAG = "k85boot";

/* tiny string helpers: the bootloader has no printf family */
/* the bootloader runs with the RTC watchdog on (~9 s): feed it whenever we wait */
#ifdef K85_BL_SIM
static void bl_feed_wdt(void) { }
#else
static void bl_feed_wdt(void)
{
    wdt_hal_context_t c = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&c);
    wdt_hal_feed(&c);
    wdt_hal_write_protect_enable(&c);
}
#endif
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int scopy(char *d, int pos, const char *s) { while (*s) d[pos++] = *s++; d[pos] = 0; return pos; }
static int sint(char *d, int pos, int v)
{
    char t[12]; int n = 0;
    if (v <= 0) t[n++] = '0';
    while (v > 0 && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) d[pos++] = t[--n];
    d[pos] = 0;
    return pos;
}

/* ---------------- board: M5StickS3 (official pin map) ---------------- */
#define K85_BTN_A_GPIO    11            /* active low */
#define K85_BTN_B_GPIO    12            /* active low */
#define K85_LCD_MOSI      39
#define K85_LCD_SCK       40
#define K85_LCD_DC        45
#define K85_LCD_CS        41
#define K85_LCD_RST       21
#define K85_LCD_BL        38            /* backlight, plain GPIO (no PMIC involved) */
/* landscape 240x135, same as k85OS (rotation 1). If the picture is upside down try
   MADCTL 0xA0 with Y offset 52; if colours are inverted set K85_LCD_INVERT to 0. */
#define K85_LCD_W         240
#define K85_LCD_H         135
#define K85_LCD_MADCTL    0x60
#define K85_LCD_XOFF      40
#define K85_LCD_YOFF      53
#define K85_LCD_INVERT    1
/* M5PM1 power IC: its GPIO2 (PYG2, active high) gates the L3B rail that powers the LCD and the backlight */
#define K85_PM1_ENABLE    1
#define K85_I2C_SDA       47
#define K85_I2C_SCL       48
#define K85_PM1_ADDR      0x6E

#define K85_HOLD_MS         600
#define K85_LOGO_MS         1200
#define K85_MENU_TIMEOUT_MS 3000
#define K85_MENU_RETURN_TIMEOUT_MS 10000   /* menu re-opened by k85OS (Rec finished / no SD card) */
#define K85_MENU_SKIP_SW  0              /* 1 = esp_restart() boots straight into the app, without the menu */
#define K85_MENU_HOLD_TIMEOUT_MS 10000     /* menu opened with A held: no endless wait, even if the LCD stays dark */
#define K85_PROJECT_NAME  "k85os_idf"
#define K85_IMG_VALID     0x2U
#define K85_REQ_OFFSET    0x12000U       /* spare sector: otadata ends at 0x12000, ota_0 starts at 0x20000 */
#define K85_REQ_SIZE      0x1000U
#define K85_REQ_MAGIC     0x4B38354DU    /* 'K85M' */
#define K85_REQ_CMD_BASE  0x10U          /* cmd = base + choice (0 normal, 1 BIOS, 2 Test, 4 Rec) */
#define K85_REQ_CMD_MENU  0x20U          /* written by k85OS: show this menu again on next boot */
#define K85_REQ_CMD_MENU_ALT 0x21U       /* same, with Alt Firmware highlighted (just flashed from SD) */
#define K85_ITEMS         7
#define K85_ITEM_COLORS   5              /* "Custom colors": handled inside the menu */
#define K85_ITEM_USB      6              /* "USB Download": reboot into the ROM download mode */
#define K85_BRESET_MS     1500           /* K85BL11: B held this long at power-on = erase otadata, k85OS boots */
#define K85_PM1_VBAT_L    0x22           /* K85BL11: M5PM1 battery voltage, 16 bit little endian, mV (read only; verify on the first flash) */
#define K85_UI_OFFSET     0x13000U       /* colours chosen in the "Custom colors" entry */
#define K85_UI_MAGIC      0x4B383553U    /* 'K85S' */
#define K85_LG_OFFSET     0x14000U       /* K85BL12: crash-loop guard log: spare sector, append-only words */
#define K85_LG_CRASH      0x4B4C0001U    /* one boot after a crash reset */
#define K85_LG_CLEAR      0x4B4C0002U    /* a normal boot after crashes: the streak is over */
#define K85_LG_LIMIT      3              /* crash resets in a row => back to k85OS, the menu stays open */
#define K85_LG_WORDS      (K85_REQ_SIZE / 4U)
static bool s_lg_trip = false;           /* the guard fired on this boot: warning in the menu, no auto-boot */

void bootloader_hooks_include(void) { }  /* makes the linker keep this file */

/* ---------------- buttons ---------------- */
static void btn_init(int g)
{
    esp_rom_gpio_pad_select_gpio(g);
    gpio_ll_input_enable(&GPIO, g);
    gpio_ll_pullup_en(&GPIO, g);
}
static bool btn_down(int g) { return gpio_ll_get_level(&GPIO, g) == 0; }

static bool button_a_held(void)
{
    btn_init(K85_BTN_A_GPIO);
    esp_rom_delay_us(5000);
    for (int i = 0; i < K85_HOLD_MS / 10; i++) {
        if (!btn_down(K85_BTN_A_GPIO)) return false;   /* released */
        esp_rom_delay_us(10000);
    }
    return true;
}

/* ---------------- LCD (ST7789P3, bit-banged SPI) ---------------- */
#define RGB(c) ((uint16_t)((((c) >> 8) & 0xF800) | (((c) >> 5) & 0x07E0) | (((c) >> 3) & 0x001F)))
static uint32_t c_bg888 = 0x000000, c_fg888 = 0xC8C8C8, c_acc888 = 0xFFA500;   /* editable (Custom colors) */
static uint16_t g_bg = RGB(0x000000), g_fg = RGB(0xC8C8C8), g_acc = RGB(0xFFA500);
#define C_BG     g_bg
#define C_FG     g_fg
#define C_GRAY   RGB(0x707070)
#define C_ACC    g_acc
#define C_BLACK  RGB(0x000000)

#ifdef K85_BL_SIM
extern uint16_t sim_fb[K85_LCD_W * K85_LCD_H];
extern int sim_x, sim_y, sim_x0, sim_x1, sim_y1;
static void lcd_win_begin(int x0, int y0, int x1, int y1) { sim_x0 = sim_x = x0; sim_y = y0; sim_x1 = x1; sim_y1 = y1; }
static inline void lcd_push(uint16_t c)
{
    if (sim_y <= sim_y1 && sim_x >= 0 && sim_x < K85_LCD_W && sim_y >= 0 && sim_y < K85_LCD_H) sim_fb[sim_y * K85_LCD_W + sim_x] = c;
    if (++sim_x > sim_x1) { sim_x = sim_x0; sim_y++; }
}
static void lcd_win_end(void) { }
static void lcd_init(void) { }
static void delay_ms(int ms) { (void)ms; }
#else
static inline void pin_out(int g, int lvl) { gpio_ll_set_level(&GPIO, g, lvl); }

static void lcd_pin(int g, int lvl)
{
    esp_rom_gpio_pad_select_gpio(g);
    esp_rom_gpio_connect_out_signal(g, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, g);
    pin_out(g, lvl);
}

static void delay_ms(int ms) { bl_feed_wdt(); esp_rom_delay_us((uint32_t)ms * 1000U); }

static inline void spi_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        pin_out(K85_LCD_SCK, 0);
        pin_out(K85_LCD_MOSI, (b >> i) & 1);
        pin_out(K85_LCD_SCK, 1);
    }
    pin_out(K85_LCD_SCK, 0);
}

static void lcd_cmd(uint8_t c)
{
    pin_out(K85_LCD_CS, 0); pin_out(K85_LCD_DC, 0);
    spi_byte(c);
    pin_out(K85_LCD_CS, 1);
}

static void lcd_cmd_data(uint8_t c, const uint8_t *d, int n)
{
    lcd_cmd(c);
    pin_out(K85_LCD_CS, 0); pin_out(K85_LCD_DC, 1);
    for (int i = 0; i < n; i++) spi_byte(d[i]);
    pin_out(K85_LCD_CS, 1);
}

static void lcd_win_begin(int x0, int y0, int x1, int y1)
{
    x0 += K85_LCD_XOFF; x1 += K85_LCD_XOFF; y0 += K85_LCD_YOFF; y1 += K85_LCD_YOFF;
    uint8_t c[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t r[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    lcd_cmd_data(0x2A, c, 4);
    lcd_cmd_data(0x2B, r, 4);
    lcd_cmd(0x2C);
    pin_out(K85_LCD_CS, 0); pin_out(K85_LCD_DC, 1);     /* stays open for lcd_push() */
}
static inline void lcd_push(uint16_t c) { spi_byte((uint8_t)(c >> 8)); spi_byte((uint8_t)c); }
static void lcd_win_end(void) { pin_out(K85_LCD_CS, 1); }

/* ---------------- M5PM1: switch the LCD rail (L3B) on. Bit-banged I2C, ~100 kHz (the PM1 NAKs 400 kHz) ---------------- */
static void i2c_pin(int g)
{
    esp_rom_gpio_pad_select_gpio(g);
    esp_rom_gpio_connect_out_signal(g, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_input_enable(&GPIO, g);
    gpio_ll_pullup_en(&GPIO, g);
    gpio_ll_set_level(&GPIO, g, 0);
    gpio_ll_output_disable(&GPIO, g);                 /* released: high through the pull-up */
}
static inline void i2c_lo(int g) { gpio_ll_output_enable(&GPIO, g); }
static inline void i2c_rel(int g)
{
    gpio_ll_output_disable(&GPIO, g);
    if (g == K85_I2C_SCL) {                           /* clock stretching: wait (max ~1 ms) until the slave lets SCL go high */
        for (int i = 0; i < 200 && gpio_ll_get_level(&GPIO, g) == 0; i++) esp_rom_delay_us(5);
    }
}
static inline void i2c_wait(void) { esp_rom_delay_us(5); }

static void i2c_start(void)
{
    i2c_rel(K85_I2C_SDA); i2c_rel(K85_I2C_SCL); i2c_wait();
    i2c_lo(K85_I2C_SDA); i2c_wait();
    i2c_lo(K85_I2C_SCL); i2c_wait();
}
static void i2c_stop(void)
{
    i2c_lo(K85_I2C_SDA); i2c_wait();
    i2c_rel(K85_I2C_SCL); i2c_wait();
    i2c_rel(K85_I2C_SDA); i2c_wait();
}
static bool i2c_wbyte(uint8_t b)                      /* true = ACK */
{
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1) i2c_rel(K85_I2C_SDA); else i2c_lo(K85_I2C_SDA);
        i2c_wait(); i2c_rel(K85_I2C_SCL); i2c_wait(); i2c_lo(K85_I2C_SCL);
    }
    i2c_rel(K85_I2C_SDA); i2c_wait();
    i2c_rel(K85_I2C_SCL); i2c_wait();
    bool ack = gpio_ll_get_level(&GPIO, K85_I2C_SDA) == 0;
    i2c_lo(K85_I2C_SCL); i2c_wait();
    return ack;
}
static uint8_t i2c_rbyte(bool ack)
{
    uint8_t v = 0;
    i2c_rel(K85_I2C_SDA);
    for (int i = 0; i < 8; i++) {
        i2c_wait(); i2c_rel(K85_I2C_SCL); i2c_wait();
        v = (uint8_t)((v << 1) | (gpio_ll_get_level(&GPIO, K85_I2C_SDA) ? 1 : 0));
        i2c_lo(K85_I2C_SCL);
    }
    if (ack) i2c_lo(K85_I2C_SDA); else i2c_rel(K85_I2C_SDA);
    i2c_wait(); i2c_rel(K85_I2C_SCL); i2c_wait(); i2c_lo(K85_I2C_SCL);
    i2c_rel(K85_I2C_SDA);
    return v;
}
static bool pm1_write(uint8_t reg, uint8_t val)
{
    i2c_start();
    bool ok = i2c_wbyte((uint8_t)(K85_PM1_ADDR << 1)) && i2c_wbyte(reg) && i2c_wbyte(val);
    i2c_stop();
    return ok;
}
static bool pm1_read(uint8_t reg, uint8_t *val)
{
    i2c_start();
    bool ok = i2c_wbyte((uint8_t)(K85_PM1_ADDR << 1)) && i2c_wbyte(reg);
    if (ok) {
        i2c_start();                                  /* repeated start */
        ok = i2c_wbyte((uint8_t)((K85_PM1_ADDR << 1) | 1));
        if (ok) *val = i2c_rbyte(false);
    }
    i2c_stop();
    return ok;
}
static bool pm1_update(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)   /* read-modify-write */
{
    uint8_t v;
    if (!pm1_read(reg, &v)) return false;
    return pm1_write(reg, (uint8_t)((v & ~clear_mask) | set_mask));
}

/* PM1 GPIO block (M5PM1 / M5Unified): 0x10 mode (1 = output), 0x11 output latch, 0x13 drive (1 = open drain),
   0x16 function (2 bits per pin, 0 = GPIO). GPIO2 drives the L3B rail. */
/* write a PM1 register field and read it back; retry a few times */
static bool pm1_update_v(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    for (int i = 0; i < 3; i++) {
        pm1_update(reg, clear_mask, set_mask);
        uint8_t v;
        if (pm1_read(reg, &v) && (v & clear_mask) == 0 && (v & set_mask) == set_mask) return true;
        esp_rom_delay_us(2000);
    }
    return false;
}

/* a reset in the middle of an I2C transfer can leave the PM1 holding SDA low: clock it out, then send a STOP */
static void i2c_recover(void)
{
    i2c_rel(K85_I2C_SDA);
    for (int i = 0; i < 9 && gpio_ll_get_level(&GPIO, K85_I2C_SDA) == 0; i++) {
        i2c_lo(K85_I2C_SCL); i2c_wait();
        i2c_rel(K85_I2C_SCL); i2c_wait();
    }
    i2c_stop();
}

static void pm1_lcd_power_on(void)
{
#if K85_PM1_ENABLE
    i2c_pin(K85_I2C_SDA); i2c_pin(K85_I2C_SCL);
    i2c_recover();
    uint8_t id = 0xFF;
    bool alive = false;
    for (int i = 0; i < 30 && !alive; i++) {          /* the first access wakes the PM1 from its idle sleep */
        alive = pm1_read(0x00, &id);
        if (!alive) esp_rom_delay_us(4000);
    }
    if (!alive) { ESP_LOGW(TAG, "M5PM1 does not answer on I2C: LCD rail not switched"); return; }
    pm1_write(0x09, 0x00);                            /* I2C idle sleep off */
    pm1_write(0x0A, 0x00);                            /* watchdog off */
    pm1_update_v(0x16, 0x30, 0x00);                     /* GPIO2 = plain GPIO */
    pm1_update_v(0x13, 0x04, 0x00);                     /* push-pull */
    pm1_update_v(0x10, 0x00, 0x04);                     /* output */
    pm1_update_v(0x11, 0x00, 0x04);                     /* high -> L3B on */
    esp_rom_delay_us(40000);                          /* let the rail settle before the panel reset */
    uint8_t m = 0, o = 0, f = 0, id1 = 0;
    pm1_read(0x10, &m); pm1_read(0x11, &o); pm1_read(0x16, &f); pm1_read(0x01, &id1);
    ESP_LOGW(TAG, "M5PM1 id=0x%x/0x%x mode=0x%x out=0x%x func=0x%x l3b=%d", (unsigned)id, (unsigned)id1, (unsigned)m, (unsigned)o, (unsigned)f, (o & 4) ? 1 : 0);
#endif
}

static void lcd_init(void)
{
    pm1_lcd_power_on();                               /* without L3B the panel is simply not powered */
    lcd_pin(K85_LCD_CS, 1); lcd_pin(K85_LCD_DC, 1); lcd_pin(K85_LCD_SCK, 0);
    lcd_pin(K85_LCD_MOSI, 0); lcd_pin(K85_LCD_RST, 1); lcd_pin(K85_LCD_BL, 0);
    delay_ms(10); pin_out(K85_LCD_RST, 0); delay_ms(20); pin_out(K85_LCD_RST, 1); delay_ms(130);
    lcd_cmd(0x01); delay_ms(150);                     /* software reset */
    lcd_cmd(0x11); delay_ms(130);                     /* sleep out */
    { uint8_t v = 0x55; lcd_cmd_data(0x3A, &v, 1); }  /* 16 bit colour */
    { uint8_t v = K85_LCD_MADCTL; lcd_cmd_data(0x36, &v, 1); }
#if K85_LCD_INVERT
    lcd_cmd(0x21);
#endif
    lcd_cmd(0x13);                                    /* normal display mode */
}
#endif /* K85_BL_SIM */

static void lcd_fill(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    lcd_win_begin(x, y, x + w - 1, y + h - 1);
    for (int i = w * h; i > 0; i--) lcd_push(c);
    lcd_win_end();
}

/* ---------------- 5x7 font (ASCII 32..126, 5 columns, bit0 = top row) ---------------- */
static const uint8_t k_font[95][5] = {
{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x05,0x03,0x00,0x00},
{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
{0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
{0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08}
};

/* text with its own background; sc = 1 (6x8 cell) or 2 (12x16 cell) */
static void lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sc)
{
    for (; *s; s++, x += 6 * sc) {
        unsigned char ch = (unsigned char)*s;
        const uint8_t *g = k_font[(ch >= 32 && ch <= 126) ? ch - 32 : '?' - 32];
        lcd_win_begin(x, y, x + 6 * sc - 1, y + 8 * sc - 1);
        for (int row = 0; row < 8 * sc; row++) {
            int gr = row / sc;
            for (int col = 0; col < 6 * sc; col++) {
                int gc = col / sc;
                lcd_push((gc < 5 && gr < 7 && ((g[gc] >> gr) & 1)) ? fg : bg);
            }
        }
        lcd_win_end();
    }
}

/* ---------------- K85BL11: status bar, battery (M5PM1 VBAT, read only) ---------------- */
#ifdef K85_BL_SIM
static void draw_battery(void) { }
#else
static int batt_pct(int mv)
{
    static const uint16_t v[8] = { 3300, 3600, 3700, 3800, 3900, 4000, 4100, 4200 };
    static const uint8_t  p[8] = {    0,    8,   20,   40,   60,   78,   90,  100 };
    if (mv <= v[0]) return 0;
    if (mv >= v[7]) return 100;
    for (int i = 1; i < 8; i++)
        if (mv < v[i]) return p[i - 1] + (p[i] - p[i - 1]) * (mv - v[i - 1]) / (v[i] - v[i - 1]);
    return 100;
}

static void draw_battery(void)
{
    uint8_t lo = 0, hi = 0;
    int mv = 0;
    if (pm1_read(K85_PM1_VBAT_L, &lo) && pm1_read((uint8_t)(K85_PM1_VBAT_L + 1), &hi)) mv = lo | (hi << 8);
    char t[24];
    int n, pct = -1;
    if (mv < 2500 || mv > 4600) {
        n = scopy(t, 0, "BAT --");
    } else {
        pct = batt_pct(mv);
        n = sint(t, 0, pct);
        n = scopy(t, n, "% ");
        n = sint(t, n, mv / 1000);
        n = scopy(t, n, ".");
        int c = (mv % 1000) / 10;
        if (c < 10) n = scopy(t, n, "0");
        n = sint(t, n, c);
        n = scopy(t, n, "V");
    }
    char u[16];
    int k = 0;
    for (int i = n; i < 10; i++) u[k++] = ' ';
    for (int i = 0; i < n && k < 15; i++) u[k++] = t[i];
    u[k] = 0;
    lcd_text(236 - 6 * 10, 3, u, (pct >= 0 && pct <= 15) ? C_ACC : C_FG, C_BG, 1);
}
#endif
/* ---------------- capybara (same shapes as k85OS boot logo), evaluated per pixel ---------------- */
static int in_circle(int px, int py, int cx, int cy, int r)
{
    int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}
static int in_rrect(int px, int py, int x, int y, int w, int h, int r)
{
    if (px < x || px >= x + w || py < y || py >= y + h) return 0;
    int qx = px < x + r ? x + r : (px > x + w - 1 - r ? x + w - 1 - r : px);
    int qy = py < y + r ? y + r : (py > y + h - 1 - r ? y + h - 1 - r : py);
    int dx = px - qx, dy = py - qy;
    return dx * dx + dy * dy <= r * r;
}
static int tri_side(int px, int py, int ax, int ay, int bx, int by)
{
    return (px - bx) * (ay - by) - (ax - bx) * (py - by);
}
static int in_tri(int px, int py, int ax, int ay, int bx, int by, int cx, int cy)
{
    int d1 = tri_side(px, py, ax, ay, bx, by), d2 = tri_side(px, py, bx, by, cx, cy), d3 = tri_side(px, py, cx, cy, ax, ay);
    int neg = (d1 < 0) || (d2 < 0) || (d3 < 0), pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}
static int on_seg(int px, int py, int x0, int y0, int x1, int y1)      /* within ~0.5 px of the segment */
{
    int vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
    int len2 = vx * vx + vy * vy, dot = wx * vx + wy * vy, cross = wx * vy - wy * vx;
    return dot >= 0 && dot <= len2 && 4 * cross * cross <= len2;
}

static uint16_t capy_px(int x, int y, int cx, int cy)
{
    const uint16_t BODY = RGB(0x8B6F47), MUZ = RGB(0xE8D5B0), DARK = RGB(0x3A2E20), ORA = RGB(0xFFA500), SEG = RGB(0xFFF3D6);
    int ox = cx, oy = cy - 34;
    int dx = x - ox, dy = y - oy, d2 = dx * dx + dy * dy;
    if (d2 >= 100 - 10 && d2 <= 100 + 10) return SEG;                       /* ring r=10 */
    for (int i = -2; i <= 2; i++) if (on_seg(x, y, ox, oy, ox + i * 3, oy - 9)) return SEG;
    if (d2 <= 100) return ORA;
    if (in_tri(x, y, cx - 4, cy + 2, cx + 4, cy + 2, cx, cy + 7)) return DARK;
    if (in_rrect(x, y, cx - 12, cy - 8, 6, 3, 1) || in_rrect(x, y, cx + 6, cy - 8, 6, 3, 1)) return DARK;
    if (in_rrect(x, y, cx - 16, cy - 2, 32, 18, 8)) return MUZ;
    if (in_circle(x, y, cx - 16, cy - 26, 4) || in_circle(x, y, cx + 16, cy - 26, 4)) return DARK;
    if (in_circle(x, y, cx - 16, cy - 26, 7) || in_circle(x, y, cx + 16, cy - 26, 7)) return BODY;
    if (in_circle(x, y, cx, cy - 6, 26)) return BODY;
    if (in_rrect(x, y, cx - 32, cy + 6, 64, 30, 14)) return BODY;
    return C_BG;
}

static void draw_logo(void)
{
    int cx = K85_LCD_W / 2, cy = K85_LCD_H / 2 - 10;
    int x0 = cx - 34, x1 = cx + 34, y0 = cy - 46, y1 = cy + 38;
    lcd_win_begin(x0, y0, x1, y1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) lcd_push(capy_px(x, y, cx, cy));
    lcd_win_end();
    lcd_text((K85_LCD_W - 5 * 6) / 2, K85_LCD_H / 2 + 44, "k85OS", C_ACC, C_BG, 1);
}

/* ---------------- boot menu ---------------- */
static const char *const k_items[K85_ITEMS] = { "k85OS", "k85OS BIOS", "Test Mode", "Alt Firmware", "Rec (SD card)", "Custom colors", "USB Download" };

static void draw_item(int i, bool sel, bool en)
{
    int y = 13 + i * 16;
    uint16_t b = sel ? C_ACC : C_BG, f = sel ? C_BG : (en ? C_FG : C_GRAY);
    char line[40];
    line[0] = sel ? '>' : ' '; line[1] = ' '; line[2] = 0;
    scopy(line, 2, k_items[i]);
    lcd_fill(4, y, 232, 16, b);
    lcd_text(10, y, line, f, b, 2);
}

static void draw_countdown(int sec)
{
    lcd_fill(110, 125, 128, 10, C_BG);
    if (sec > 0) {
        char t[40];
        int n = scopy(t, 0, "auto-boot in ");
        n = sint(t, n, sec);
        scopy(t, n, "s");
        lcd_text(236 - 6 * slen(t), 126, t, C_GRAY, C_BG, 1);
    }
}

static void draw_battery(void);

static void draw_menu_static(void)
{
    lcd_fill(0, 0, K85_LCD_W, K85_LCD_H, C_BG);
    lcd_text(8, 3, s_lg_trip ? "CRASH LOOP x3: choose boot" : "k85OS boot loader", C_ACC, C_BG, 1);
    lcd_fill(4, 12, 232, 1, C_GRAY);
    lcd_text(4, 126, "A=next B=select", C_GRAY, C_BG, 1);
    draw_battery();
}

/* ---------------- "Custom colors": background / text / accent ---------------- */
static const uint32_t k_pal[16] = {
    0x000000, 0x202020, 0x101830, 0x1E5AFF, 0x008080, 0x00A650, 0x80FF00, 0xFFE000,
    0xFF8800, 0xFF3030, 0xFF00FF, 0x8040FF, 0x00FFFF, 0xFF80C0, 0xC8C8C8, 0xFFFFFF
};
static const char *const k_crow[6] = { "Background", "Text", "Accent", "Reset to default", "Save and back", "Cancel" };

static void apply_colors(void) { g_bg = RGB(c_bg888); g_fg = RGB(c_fg888); g_acc = RGB(c_acc888); }

static void draw_custom_row(int i, bool sel)
{
    int y = 18 + i * 16;
    uint16_t b = sel ? C_ACC : C_BG, f = sel ? C_BG : C_FG;
    lcd_fill(4, y, 232, 14, b);
    lcd_text(10, y + 3, k_crow[i], f, b, 1);
    if (i < 3) {                                          /* colour swatch with an outline */
        uint32_t c = i == 0 ? c_bg888 : (i == 1 ? c_fg888 : c_acc888);
        lcd_fill(170, y + 1, 62, 12, C_GRAY);
        lcd_fill(171, y + 2, 60, 10, RGB(c));
    }
}

static void draw_custom(int row)
{
    lcd_fill(0, 0, K85_LCD_W, K85_LCD_H, C_BG);
    lcd_text(8, 3, "Custom colors", C_ACC, C_BG, 1);
    lcd_fill(4, 12, 232, 1, C_GRAY);
    for (int i = 0; i < 6; i++) draw_custom_row(i, i == row);
    lcd_text(4, 123, "A=next B=change", C_GRAY, C_BG, 1);
}

/* next palette colour for background (0) / text (1) / accent (2); text and accent must differ from the background */
static void cycle_color(int which)
{
    uint32_t *c = which == 0 ? &c_bg888 : (which == 1 ? &c_fg888 : &c_acc888);
    int idx = -1;
    for (int k = 0; k < 16; k++) if (k_pal[k] == *c) idx = k;
    for (int n = 0; n < 16; n++) {
        idx = (idx + 1) & 15;
        uint16_t v = RGB(k_pal[idx]);
        if (which != 0 && v == RGB(c_bg888)) continue;
        if (which == 0 && (v == RGB(c_fg888) || v == RGB(c_acc888))) continue;
        *c = k_pal[idx];
        break;
    }
    apply_colors();
}

static void custom_colors(void);                         /* defined below (needs flash access; a stub in the simulator) */
#ifdef K85_BL_SIM
static void custom_colors(void) { }
#endif

/* returns the chosen entry 0..4 (the colour editor, entry 5, runs inside). wait_forever: no auto-boot */
static int run_menu(int initial, bool wait_forever, int timeout_ms, const bool enabled[K85_ITEMS])
{
    draw_menu_static();
    int sel = initial;
    for (int i = 0; i < K85_ITEMS; i++) draw_item(i, i == sel, enabled[i]);
    btn_init(K85_BTN_A_GPIO); btn_init(K85_BTN_B_GPIO);
    bool pa = btn_down(K85_BTN_A_GPIO), pb = btn_down(K85_BTN_B_GPIO);
    bool interacted = wait_forever;
    int ms = 0, last_sec = -1;
    while (1) {
        bool a = btn_down(K85_BTN_A_GPIO), b = btn_down(K85_BTN_B_GPIO);
        if (a && !pa) {
            interacted = true;
            int old = sel;
            do { sel = (sel + 1) % K85_ITEMS; } while (!enabled[sel]);
            draw_item(old, false, enabled[old]);
            draw_item(sel, true, enabled[sel]);
            draw_countdown(0);
        }
        if (b && !pb) {
            if (sel != K85_ITEM_COLORS) return sel;
            custom_colors();                                  /* colour editor; back here afterwards */
            draw_menu_static();
            for (int i = 0; i < K85_ITEMS; i++) draw_item(i, i == sel, enabled[i]);
            interacted = true;                                /* no auto-boot after editing */
            pa = btn_down(K85_BTN_A_GPIO); pb = btn_down(K85_BTN_B_GPIO);
            continue;
        }
        pa = a; pb = b;
        if (!interacted) {
            int left = (timeout_ms - ms + 999) / 1000;
            if (left != last_sec) { draw_countdown(left); last_sec = left; }
            if (ms >= timeout_ms) return sel;
        }
        esp_rom_delay_us(10000);
        ms += 10;
        if (ms % 3000 == 1500) draw_battery();
        bl_feed_wdt();
    }
}

#ifndef K85_BL_SIM
/* ---------------- app slots / boot selection ---------------- */
typedef struct {
    uint32_t otadata_off, otadata_size;
    uint32_t slot_off[16];
    bool slot_present[16];
    int ota_count;
    int k85_slot;          /* slot holding k85OS, -1 unless exactly one */
    int alt_slot;          /* first other slot with a valid image, -1 if none */
    int cur_slot;          /* slot selected by otadata */
} slots_t;

/* app image: 24 byte header + 8 byte segment header, then esp_app_desc_t (project_name at +48) */
static int slot_kind(uint32_t offset)      /* 0 = no image, 1 = k85OS, 2 = other firmware */
{
    uint8_t b[112];
    if (bootloader_flash_read(offset, b, sizeof(b), false) != ESP_OK) return 0;
    if (b[0] != 0xE9) return 0;
    uint32_t magic;
    memcpy(&magic, b + 32, sizeof(magic));
    if (magic == 0xABCD5432U && strncmp((const char *)(b + 80), K85_PROJECT_NAME, 32) == 0) return 1;
    return 2;
}

static bool scan_slots(slots_t *s)
{
    memset(s, 0, sizeof(*s));
    s->k85_slot = s->alt_slot = s->cur_slot = -1;
    esp_partition_info_t info;
    for (int i = 0; i < 95; i++) {
        if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET + (uint32_t)i * sizeof(info), &info, sizeof(info), false) != ESP_OK) break;
        if (info.magic != ESP_PARTITION_MAGIC) break;
        if (info.type == PART_TYPE_APP && (info.subtype & 0x10) == 0x10) {
            int idx = info.subtype & 0x0F;
            s->slot_off[idx] = info.pos.offset;
            s->slot_present[idx] = true;
            s->ota_count++;
        } else if (info.type == PART_TYPE_DATA && info.subtype == PART_SUBTYPE_DATA_OTA) {
            s->otadata_off = info.pos.offset;
            s->otadata_size = info.pos.size;
        }
    }
    int k85_count = 0, first_k85 = -1;
    int kinds[16];
    for (int i = 0; i < 16; i++) {
        kinds[i] = s->slot_present[i] ? slot_kind(s->slot_off[i]) : 0;
        if (kinds[i] == 1) { k85_count++; if (first_k85 < 0) first_k85 = i; }
        else if (kinds[i] == 2 && s->alt_slot < 0) s->alt_slot = i;
    }
    if (k85_count < 1 || s->otadata_off == 0 || s->otadata_size < 0x2000 || s->ota_count < 1) {
        ESP_LOGW(TAG, "k85OS found in %d slot(s): boot selection left untouched", k85_count);
        return false;
    }
    s->k85_slot = first_k85;
    esp_ota_select_entry_t two[2];
    if (bootloader_flash_read(s->otadata_off, &two[0], sizeof(two[0]), false) == ESP_OK &&
        bootloader_flash_read(s->otadata_off + 0x1000, &two[1], sizeof(two[1]), false) == ESP_OK) {
        int act = bootloader_common_get_active_otadata(two);
        if (act >= 0) s->cur_slot = (int)((two[act].ota_seq - 1U) % (uint32_t)s->ota_count);
    }
    if (s->cur_slot < 0 || !s->slot_present[s->cur_slot]) {
        for (int i = 0; i < 16; i++) if (s->slot_present[i]) { s->cur_slot = i; break; }   /* IDF default: first slot */
    }
    if (kinds[s->cur_slot] == 1) s->k85_slot = s->cur_slot;     /* two k85OS builds installed: keep the selected one */
    return true;
}

static bool write_boot_slot(const slots_t *s, int slot)
{
    esp_ota_select_entry_t e;
    memset(&e, 0xFF, sizeof(e));
    e.ota_seq = (uint32_t)(slot + 1);
    e.ota_state = K85_IMG_VALID;
    e.crc = bootloader_common_ota_select_crc(&e);
    if (bootloader_flash_erase_range(s->otadata_off, s->otadata_size) != ESP_OK) { ESP_LOGE(TAG, "otadata erase failed"); return false; }
    if (bootloader_flash_write(s->otadata_off, &e, sizeof(e), false) != ESP_OK) { ESP_LOGE(TAG, "otadata write failed"); return false; }
    ESP_LOGW(TAG, "boot selection set to ota_%d", slot);
    return true;
}

/* a spare sector must not belong to any partition of this table */
static bool sector_free(uint32_t off)
{
    esp_partition_info_t info;
    for (int i = 0; i < 95; i++) {
        if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET + (uint32_t)i * sizeof(info), &info, sizeof(info), false) != ESP_OK) return false;
        if (info.magic != ESP_PARTITION_MAGIC) break;
        uint32_t a = info.pos.offset, b = info.pos.offset + info.pos.size;
        if (a < off + K85_REQ_SIZE && b > off) return false;
    }
    return true;
}
static bool request_sector_free(void) { return sector_free(K85_REQ_OFFSET); }

/* colours saved by the "Custom colors" editor */
static void load_ui(void)
{
    uint32_t r[8];
    if (!sector_free(K85_UI_OFFSET)) return;
    if (bootloader_flash_read(K85_UI_OFFSET, r, sizeof(r), false) != ESP_OK) return;
    if (r[0] != K85_UI_MAGIC || r[5] != ~K85_UI_MAGIC) return;
    c_bg888 = r[2] & 0xFFFFFFU; c_fg888 = r[3] & 0xFFFFFFU; c_acc888 = r[4] & 0xFFFFFFU;
    apply_colors();
    if (g_fg == g_bg || g_acc == g_bg) {                  /* unreadable combination: back to the defaults */
        c_bg888 = 0x000000; c_fg888 = 0xC8C8C8; c_acc888 = 0xFFA500;
        apply_colors();
    }
}

static void ui_save(void)
{
    if (!sector_free(K85_UI_OFFSET)) return;
    uint32_t rec[8] = { K85_UI_MAGIC, 0U, c_bg888, c_fg888, c_acc888, ~K85_UI_MAGIC, 0U, 0U };
    if (bootloader_flash_erase_range(K85_UI_OFFSET, K85_REQ_SIZE) != ESP_OK) return;
    bootloader_flash_write(K85_UI_OFFSET, rec, sizeof(rec), false);
}

static void wait_buttons_released(void)
{
    for (int i = 0; i < 100 && (btn_down(K85_BTN_A_GPIO) || btn_down(K85_BTN_B_GPIO)); i++) { esp_rom_delay_us(10000); bl_feed_wdt(); }
}

/* A = next row, B = change the colour / run the action. No timeout while editing. */
static void custom_colors(void)
{
    uint32_t sb = c_bg888, sf = c_fg888, sa = c_acc888;   /* for "Cancel" */
    int row = 0;
    wait_buttons_released();
    draw_custom(row);
    bool pa = false, pb = false;
    while (1) {
        bool a = btn_down(K85_BTN_A_GPIO), b = btn_down(K85_BTN_B_GPIO);
        if (a && !pa) {
            int old = row;
            row = (row + 1) % 6;
            draw_custom_row(old, false);
            draw_custom_row(row, true);
        }
        if (b && !pb) {
            if (row < 3) { cycle_color(row); draw_custom(row); }
            else if (row == 3) { c_bg888 = 0x000000; c_fg888 = 0xC8C8C8; c_acc888 = 0xFFA500; apply_colors(); draw_custom(row); }
            else {
                if (row == 4) ui_save();
                else { c_bg888 = sb; c_fg888 = sf; c_acc888 = sa; apply_colors(); }
                wait_buttons_released();
                return;
            }
        }
        pa = a; pb = b;
        esp_rom_delay_us(10000);
        bl_feed_wdt();
    }
}

static void request_write(uint32_t cmd)
{
    if (!request_sector_free()) { ESP_LOGW(TAG, "request sector is used by a partition: record skipped"); return; }
    uint32_t rec[4] = { K85_REQ_MAGIC, cmd, ~K85_REQ_MAGIC, 0U };
    if (bootloader_flash_erase_range(K85_REQ_OFFSET, K85_REQ_SIZE) != ESP_OK) { ESP_LOGE(TAG, "request erase failed"); return; }
    if (bootloader_flash_write(K85_REQ_OFFSET, rec, sizeof(rec), false) != ESP_OK) { ESP_LOGE(TAG, "request write failed"); return; }
}

static void request_clear(void)     /* a stale record must not leak into a later k85OS start */
{
    uint32_t r[1];
    if (!request_sector_free()) return;
    if (bootloader_flash_read(K85_REQ_OFFSET, r, sizeof(r), false) != ESP_OK) return;
    if (r[0] == K85_REQ_MAGIC) bootloader_flash_erase_range(K85_REQ_OFFSET, K85_REQ_SIZE);
}

static uint32_t request_peek(void)      /* command left by k85OS, 0 if none */
{
    uint32_t r[4];
    if (!request_sector_free()) return 0;
    if (bootloader_flash_read(K85_REQ_OFFSET, r, sizeof(r), false) != ESP_OK) return 0;
    if (r[0] != K85_REQ_MAGIC || r[2] != ~K85_REQ_MAGIC) return 0;
    return r[1];
}

/* K85BL12: crash-loop guard. A crash reset = panic (IDF leaves ESP_RST_PANIC = 4 in RTC STORE6 as the reset hint) or a watchdog reset. */
static bool lg_crash_reset(int rr)
{
    uint32_t h = REG_READ(RTC_CNTL_STORE6_REG);
    if ((h & 0x80000000U) && (h & 0x7FFFU) == 4U && ((h >> 16) & 0x7FFFU) == 4U) return true;
    return rr == 7 || rr == 8 || rr == 9 || rr == 11 || rr == 13 || rr == 16 || rr == 17 || rr == 18;
}

/* first free word of the log and the crash streak since the last CLEAR */
static bool lg_scan(uint32_t *pos, int *streak)
{
    uint32_t w[32];
    *pos = K85_LG_WORDS;
    *streak = 0;
    if (!sector_free(K85_LG_OFFSET)) return false;
    for (uint32_t base = 0; base < K85_LG_WORDS; base += 32) {
        if (bootloader_flash_read(K85_LG_OFFSET + base * 4U, w, sizeof(w), false) != ESP_OK) return false;
        for (int i = 0; i < 32; i++) {
            if (w[i] == 0xFFFFFFFFU) { *pos = base + (uint32_t)i; return true; }
            if (w[i] == K85_LG_CLEAR) *streak = 0;
            else if (w[i] == K85_LG_CRASH) (*streak)++;
        }
    }
    return true;
}

static void lg_append(uint32_t *pos, uint32_t mark)
{
    if (*pos >= K85_LG_WORDS) {                             /* log full: start again (once per ~1000 events) */
        if (bootloader_flash_erase_range(K85_LG_OFFSET, K85_REQ_SIZE) != ESP_OK) return;
        *pos = 0;
    }
    if (bootloader_flash_write(K85_LG_OFFSET + *pos * 4U, &mark, 4, false) == ESP_OK) (*pos)++;
}

/* called on every start: returns the crash streak including this boot */
static int loop_guard_update(int rr)
{
    uint32_t pos;
    int streak;
    if (!lg_scan(&pos, &streak)) return 0;
    if (lg_crash_reset(rr)) {
        if (pos >= K85_LG_WORDS) streak = 0;
        lg_append(&pos, K85_LG_CRASH);
        streak++;
    } else if (streak > 0) {
        lg_append(&pos, K85_LG_CLEAR);
        streak = 0;
    }
    return streak;
}

static void loop_guard_clear(void)                           /* the guard has acted: the next boot starts a fresh streak */
{
    uint32_t pos;
    int streak;
    if (!lg_scan(&pos, &streak)) return;
    lg_append(&pos, K85_LG_CLEAR);
}

/* K85BL11: B held at power-on */
static bool button_b_held(void)
{
    btn_init(K85_BTN_B_GPIO);
    esp_rom_delay_us(5000);
    for (int i = 0; i < K85_BRESET_MS / 10; i++) {
        if (!btn_down(K85_BTN_B_GPIO)) return false;
        esp_rom_delay_us(10000);
        if (i % 100 == 0) bl_feed_wdt();
    }
    return true;
}

/* K85BL11: reboot into the ROM download mode (USB / UART): esptool can flash right away */
static void enter_usb_download(void)
{
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
    while (1) { }
}
void bootloader_after_init(void)
{
    bool a_held = button_a_held();
    if (!a_held && button_b_held()) {                   /* K85BL11: B held at power-on = boot selection back to k85OS */
        slots_t sb;
        bool found = scan_slots(&sb);
        bool done = false;
        if (found) done = write_boot_slot(&sb, sb.k85_slot);            /* erases otadata and selects the k85OS slot */
        else if (sb.otadata_off != 0 && sb.otadata_size != 0)
            done = (bootloader_flash_erase_range(sb.otadata_off, sb.otadata_size) == ESP_OK);   /* like esptool erase_region */
        if (done) request_clear();
        load_ui();
        lcd_init();
        lcd_fill(0, 0, K85_LCD_W, K85_LCD_H, C_BG);
        pin_out(K85_LCD_BL, 1);
        lcd_cmd(0x29);
        lcd_text(8, 40, "B held: boot reset", C_ACC, C_BG, 2);
        lcd_text(8, 70, done ? "otadata erased" : "otadata: FAILED", C_FG, C_BG, 1);
        lcd_text(8, 84, "starting k85OS ...", C_GRAY, C_BG, 1);
        wait_buttons_released();
        delay_ms(1200);
        return;
    }
    int rr = (int)esp_rom_get_reset_reason(0);
    /* the menu is shown on every start (K85_MENU_SKIP_SW = 1 makes esp_restart() (3, 12) boot straight away).
       Everything else (power-on 1, wake from deep sleep 5, USB resets 21 22, glitch 23 ...) shows the menu. */
    bool app_restart = (K85_MENU_SKIP_SW && (rr == 3 || rr == 12));
    ESP_LOGW(TAG, "reset reason %d", rr);
    int loops = loop_guard_update(rr);
    uint32_t req = request_peek();
    bool again = (req == K85_REQ_CMD_MENU || req == K85_REQ_CMD_MENU_ALT);   /* k85OS asked for the menu again */
    if (!a_held && !again && app_restart) return;             /* software restarts etc.: boot straight away */

    slots_t s;
    if (!scan_slots(&s)) return;                        /* nothing sensible to choose from */
    if (again) request_clear();                         /* one-shot: a dead screen must not trap the next boot */
    if (loops >= K85_LG_LIMIT) {                          /* K85BL12: crash loop => k85OS slot + the menu stays open */
        s_lg_trip = true;
        if (s.cur_slot != s.k85_slot && write_boot_slot(&s, s.k85_slot)) s.cur_slot = s.k85_slot;
        loop_guard_clear();
    }

    load_ui();
    lcd_init();
    lcd_fill(0, 0, K85_LCD_W, K85_LCD_H, C_BG);
    pin_out(K85_LCD_BL, 1);
    lcd_cmd(0x29);                                      /* display on */
    draw_logo();
    delay_ms(K85_LOGO_MS);

    bool enabled[K85_ITEMS] = { true, true, true, s.alt_slot >= 0, true, true, true };
    int initial = 0;
    if (req == K85_REQ_CMD_MENU_ALT && s.alt_slot >= 0) initial = 3;
    else if (!a_held && !again && s.cur_slot != s.k85_slot && s.alt_slot >= 0) initial = 3;
    if (s_lg_trip) initial = 0;
    int choice = run_menu(initial, s_lg_trip, a_held ? K85_MENU_HOLD_TIMEOUT_MS : (again ? K85_MENU_RETURN_TIMEOUT_MS : K85_MENU_TIMEOUT_MS), enabled);
    ESP_LOGW(TAG, "boot menu choice %d", choice);
    if (choice == K85_ITEM_USB) {                       /* K85BL11: ROM download mode over USB */
        lcd_fill(0, 0, K85_LCD_W, K85_LCD_H, C_BG);
        lcd_text(8, 40, "USB download", C_ACC, C_BG, 2);
        lcd_text(8, 66, "connect USB, run esptool", C_FG, C_BG, 1);
        lcd_text(8, 80, "esptool resets it when done", C_GRAY, C_BG, 1);
        delay_ms(1500);
        enter_usb_download();
        return;
    }

    char msg[48];
    scopy(msg, scopy(msg, 0, "booting "), k_items[choice]);
    scopy(msg, slen(msg), " ...");
    lcd_fill(0, 125, K85_LCD_W, 10, C_BG);
    lcd_text(4, 126, msg, C_ACC, C_BG, 1);

    if (choice == 3) {
        if (s.cur_slot != s.alt_slot) write_boot_slot(&s, s.alt_slot);
        request_clear();
    } else {
        if (s.cur_slot != s.k85_slot) write_boot_slot(&s, s.k85_slot);
        request_write(K85_REQ_CMD_BASE + (uint32_t)choice);
    }
}
#endif /* !K85_BL_SIM */