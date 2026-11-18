// K85RU1: Russian font (5x7 glyphs in the 6x8 cell), language switch and dictionary
#include "lang.h"
#include "svc_cfg.h"
#include "M5Unified.h"

#include <cstdio>
#include <cstring>

// ---- glyph table: 5 columns per character, bit0 = top row (same layout as the built-in font) ----
// index 1..32 = A..Ya (U+0410..U+042F), 33..64 = a..ya (U+0430..U+044F), 65 = Yo, 66 = yo
static const uint8_t k_ru_tbl[67 * 5] = {
    0x00,0x00,0x00,0x00,0x00,
    0x7E,0x11,0x11,0x11,0x7E,  /* А */  0x7F,0x49,0x49,0x49,0x31,  /* Б */  0x7F,0x49,0x49,0x49,0x36,  /* В */
    0x7F,0x01,0x01,0x01,0x01,  /* Г */  0x60,0x3F,0x21,0x3F,0x60,  /* Д */  0x7F,0x49,0x49,0x49,0x41,  /* Е */
    0x63,0x14,0x7F,0x14,0x63,  /* Ж */  0x22,0x41,0x49,0x49,0x36,  /* З */  0x7F,0x10,0x08,0x04,0x7F,  /* И */
    0x7C,0x21,0x12,0x09,0x7C,  /* Й */  0x7F,0x08,0x14,0x22,0x41,  /* К */  0x60,0x1F,0x01,0x01,0x7F,  /* Л */
    0x7F,0x02,0x04,0x02,0x7F,  /* М */  0x7F,0x08,0x08,0x08,0x7F,  /* Н */  0x3E,0x41,0x41,0x41,0x3E,  /* О */
    0x7F,0x01,0x01,0x01,0x7F,  /* П */  0x7F,0x09,0x09,0x09,0x06,  /* Р */  0x3E,0x41,0x41,0x41,0x22,  /* С */
    0x01,0x01,0x7F,0x01,0x01,  /* Т */  0x03,0x04,0x78,0x04,0x03,  /* У */  0x1C,0x22,0x7F,0x22,0x1C,  /* Ф */
    0x63,0x14,0x08,0x14,0x63,  /* Х */  0x3F,0x20,0x20,0x3F,0x60,  /* Ц */  0x0F,0x08,0x08,0x08,0x7F,  /* Ч */
    0x7F,0x40,0x7F,0x40,0x7F,  /* Ш */  0x7F,0x40,0x7F,0x40,0xFF,  /* Щ */  0x01,0x7F,0x48,0x48,0x30,  /* Ъ */
    0x7F,0x48,0x48,0x30,0x7F,  /* Ы */  0x7F,0x48,0x48,0x48,0x30,  /* Ь */  0x22,0x49,0x49,0x49,0x3E,  /* Э */
    0x7F,0x08,0x3E,0x41,0x3E,  /* Ю */  0x46,0x29,0x19,0x09,0x7F,  /* Я */
    0x20,0x54,0x54,0x54,0x78,  /* а */  0x3C,0x4A,0x49,0x49,0x30,  /* б */  0x7C,0x54,0x54,0x54,0x28,  /* в */
    0x7C,0x04,0x04,0x04,0x04,  /* г */  0x60,0x3C,0x24,0x3C,0x60,  /* д */  0x38,0x54,0x54,0x54,0x18,  /* е */
    0x44,0x28,0x7C,0x28,0x44,  /* ж */  0x28,0x44,0x54,0x54,0x28,  /* з */  0x7C,0x20,0x10,0x08,0x7C,  /* и */
    0x7C,0x21,0x12,0x09,0x7C,  /* й */  0x7C,0x10,0x28,0x44,0x00,  /* к */  0x40,0x3C,0x04,0x04,0x7C,  /* л */
    0x7C,0x08,0x10,0x08,0x7C,  /* м */  0x7C,0x10,0x10,0x10,0x7C,  /* н */  0x38,0x44,0x44,0x44,0x38,  /* о */
    0x7C,0x04,0x04,0x04,0x7C,  /* п */  0xFC,0x24,0x24,0x24,0x18,  /* р */  0x38,0x44,0x44,0x44,0x20,  /* с */
    0x04,0x04,0x7C,0x04,0x04,  /* т */  0x0C,0x50,0x50,0x50,0x3C,  /* у */  0x18,0x24,0xFF,0x24,0x18,  /* ф */
    0x44,0x28,0x10,0x28,0x44,  /* х */  0x7C,0x40,0x40,0x7C,0xC0,  /* ц */  0x1C,0x10,0x10,0x10,0x7C,  /* ч */
    0x7C,0x40,0x7C,0x40,0x7C,  /* ш */  0x7C,0x40,0x7C,0x40,0xFC,  /* щ */  0x04,0x7C,0x50,0x50,0x20,  /* ъ */
    0x7C,0x50,0x50,0x20,0x7C,  /* ы */  0x7C,0x50,0x50,0x50,0x20,  /* ь */  0x28,0x44,0x54,0x54,0x38,  /* э */
    0x7C,0x10,0x38,0x44,0x38,  /* ю */  0x48,0x34,0x14,0x14,0x7C,  /* я */
    0xFE,0x93,0x92,0x93,0x82,  /* Ё */  0x38,0x55,0x54,0x55,0x18   /* ё */
};

static int ru_code(uint16_t u) {
    if (u >= 0x410 && u <= 0x44F) return 1 + (u - 0x410);
    if (u == 0x401) return 65;
    if (u == 0x451) return 66;
    return 0;
}

// Built-in Font0 for everything else; our own table for the Russian letters (re-uses Font0's own drawing code)
struct K85Font : public lgfx::GLCDfont {
    const lgfx::GLCDfont ru;
    K85Font()
      : lgfx::GLCDfont(static_cast<const uint8_t *>(fonts::Font0.void_chartbl), fonts::Font0.widthtbl, 6, 8, 7)
      , ru(k_ru_tbl, fonts::Font0.widthtbl, 6, 8, 7) {}
    bool updateFontMetric(lgfx::FontMetrics *m, uint16_t u) const override {
        return ru_code(u) ? true : lgfx::GLCDfont::updateFontMetric(m, u);
    }
    size_t drawChar(lgfx::LGFXBase *g, int32_t x, int32_t y, uint16_t c, const lgfx::TextStyle *st, lgfx::FontMetrics *m, int32_t &fx) const override {
        int rc = ru_code(c);
        if (rc) return ru.drawChar(g, x, y, (uint16_t)rc, st, m, fx);
        return lgfx::GLCDfont::drawChar(g, x, y, c, st, m, fx);
    }
};
static K85Font s_font;
const lgfx::IFont *k85_ru_font(void) { return &s_font; }

// ---- UTF-8 helpers ----
int k85_u8len(const char *s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}
void k85_u8trunc(char *dst, size_t n, const char *src, int maxch) {
    if (!n) return;
    size_t o = 0;
    int ch = 0;
    size_t i = 0;
    while (src[i]) {
        unsigned char c = (unsigned char)src[i];
        size_t len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1)));
        if (ch >= maxch || o + len + 1 > n) break;
        size_t k = 0;
        while (k < len && src[i + k]) { dst[o++] = src[i + k]; k++; }
        i += k;
        ch++;
    }
    dst[o] = 0;
}

// ---- language ----
static int s_lang = -1;   // -1 = not read yet, 0 = EN, 1 = RU
bool k85_lang_ru(void) {
    if (s_lang < 0) {
        char v[8];
        s_lang = (k85_svc_get_str("lang", v, sizeof(v)) && !strcmp(v, "ru")) ? 1 : 0;
    }
    return s_lang == 1;
}
void k85_lang_set(bool ru) {
    s_lang = ru ? 1 : 0;
    k85_svc_set_str("lang", ru ? "ru" : "en");
}
void k85_lang_cmd(const char *arg, char *out, size_t n) {
    while (*arg == ' ') arg++;
    if (!strcmp(arg, "ru")) k85_lang_set(true);
    else if (!strcmp(arg, "en")) k85_lang_set(false);
    snprintf(out, n, "language: %s\r\nusage: lang ru|en\r\n", k85_lang_ru() ? "ru" : "en");
}

struct Tr { const char *en; const char *ru; };
static const Tr k_dict[] = {
    {"Back","Назад"}, {"Exit","Выход"}, {"OK","ОК"}, {"Cancel","Отмена"}, {"Yes","Да"}, {"No","Нет"},
    {"Settings","Настройки"}, {"Tools","Инструменты"}, {"Games","Игры"}, {"Apps","Приложения"}, {"Clock","Часы"},
    {"Browser","Браузер"}, {"Messenger","Мессенджер"}, {"Interpreter","Интерпретатор"}, {"System info","О системе"},
    {"Logs","Журнал"}, {"Notifications","Уведомления"}, {"Low tone","Низкий тон"}, {"High tone","Высокий тон"},
    {"Both tones","Оба тона"}, {"Cube","Куб"}, {"Colors","Цвета"}, {"WiFi","WiFi"}, {"Linux","Linux"},
    {"Status","Статус"}, {"Scan & connect","Поиск сети"}, {"Forget current","Забыть сеть"}, {"WiFi Manager","Менеджер WiFi"},
    {"Reboot","Перезагрузка"}, {"Refresh","Обновить"}, {"Free RAM","Свободная ОЗУ"}, {"Uptime","Время работы"},
    {"WiFi status","Статус WiFi"}, {"Command line","Командная строка"}, {"Run code line","Строка кода"},
    {"Everyone","Все"}, {"Inbox files","Принятые файлы"}, {"Send file","Отправить файл"}, {"Scroll older","Старые"},
    {"Newest","Новые"}, {"Theme","Тема"}, {"Brightness","Яркость"}, {"Rotation","Поворот"}, {"Volume","Громкость"},
    {"Sound","Звук"}, {"Language","Язык"}, {"Hotspot","Точка доступа"}, {"WiFi Hotspot","Точка доступа"},
    {"Files","Файлы"}, {"Music Player","Плеер"}, {"Terminal","Терминал"}, {"Calculator","Калькулятор"},
    {"Task Manager","Диспетчер задач"}, {"Performance","Производительность"}, {"Device Manager","Диспетчер устройств"},
    {"Services","Службы"}, {"Ping","Пинг"}, {"Bluetooth Scan","Поиск Bluetooth"}, {"I2C Scanner","Сканер I2C"},
    {"Air Mouse","Аэромышь"}, {"IR Remote","ИК-пульт"}, {"Snake","Змейка"}, {"Tetris","Тетрис"}, {"Pong","Понг"},
    {"Memory","Память"}, {"Race","Гонка"}, {"Reaction","Реакция"}, {"Flappy","Flappy"}, {"Dash","Dash"},
    {"Packages","Пакеты"}, {"Package Manager","Менеджер пакетов"}, {"Web Radio","Веб-радио"}, {"Web Terminal","Веб-терминал"},
    {"Password Manager","Пароли"}, {"Authenticator","Аутентификатор"}, {"Level","Уровень"}, {"Audio","Аудио"},
    {"Cleanup","Очистка"}, {"Disk cleanup","Очистка диска"}, {"MQTT","MQTT"}, {"FTP","FTP"}, {"Backup","Резервная копия"},
    {"USB drive","USB-диск"}, {"Power","Питание"}, {"Look","Оформление"}, {"Status bar","Строка состояния"},
    {"Lock screen","Экран блокировки"}, {"Sleep","Сон"}, {"Power Off","Выключить"}, {"Test Mode","Тест"},
};

const char *k85_tr(const char *s) {
    if (!k85_lang_ru() || !s || !s[0]) return s;
    for (size_t i = 0; i < sizeof(k_dict) / sizeof(k_dict[0]); i++) {
        if (!strcmp(k_dict[i].en, s)) return k_dict[i].ru;
    }
    return s;
}