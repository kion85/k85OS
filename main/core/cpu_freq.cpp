#include "cpu_freq.h"
#include "config.h"
#include "svc_cfg.h"
#include "esp_pm.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>

static bool apply_freq(int mhz) {
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = mhz;
    pm_config.min_freq_mhz = mhz; // жёстко фиксируем, без автоснижения в простое
    pm_config.light_sleep_enable = false;
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        k85_log("cpu_freq: esp_pm_configure(%d) failed, err=%d", mhz, (int)err);
        return false;
    }
    return true;
}

static int normalize(int mhz) {
    if (mhz != 240 && mhz != 160 && mhz != 80) return 160; // неизвестное/дефолтное значение
    return mhz;
}

// Значение из BIOS (ключ bios_cpu в k85_svc.cfg), кэшируется: BIOS перерисовывает строку часто.
static int s_bios_cpu = -1; // -1 = ещё не читали

int k85_cpu_freq_bios_override(void) {
    if (s_bios_cpu < 0) {
        char v[8];
        int m = 0;
        if (k85_svc_get_str("bios_cpu", v, sizeof(v))) m = atoi(v);
        s_bios_cpu = (m == 80 || m == 160 || m == 240) ? m : 0;
    }
    return s_bios_cpu;
}

static int effective_freq(void) {
    int o = k85_cpu_freq_bios_override();
    return o ? o : normalize(g_config.cpu_freq_mhz);
}

int k85_cpu_freq_cycle(void) {
    int o = k85_cpu_freq_bios_override();
    if (o) return o; // BIOS имеет приоритет: настройка ОС игнорируется

    int cur = normalize(g_config.cpu_freq_mhz);
    int next = (cur == 240) ? 160 : (cur == 160) ? 80 : 240;
    if (apply_freq(next)) {
        g_config.cpu_freq_mhz = next;
        k85_config_save();
    }
    return next;
}

void k85_cpu_freq_apply_saved(void) {
    apply_freq(effective_freq());
}

int k85_cpu_freq_bios_cycle(void) {
    int cur = k85_cpu_freq_bios_override();
    int next = (cur == 0) ? 80 : (cur == 80) ? 160 : (cur == 160) ? 240 : 0;
    if (next == 0) {
        k85_svc_set_str("bios_cpu", nullptr);
    } else {
        char v[8];
        snprintf(v, sizeof(v), "%d", next);
        k85_svc_set_str("bios_cpu", v);
    }
    s_bios_cpu = next;
    apply_freq(effective_freq());
    return next;
}

void k85_cpu_freq_bios_label(char *out, size_t out_size) {
    int o = k85_cpu_freq_bios_override();
    if (o) snprintf(out, out_size, "%d MHz", o);
    else snprintf(out, out_size, "OS");
}
