#include "cpu_freq.h"
#include "config.h"
#include "esp_pm.h"
#include "log.h"

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

int k85_cpu_freq_cycle(void) {
    int cur = normalize(g_config.cpu_freq_mhz);
    int next = (cur == 240) ? 160 : (cur == 160) ? 80 : 240;
    if (apply_freq(next)) {
        g_config.cpu_freq_mhz = next;
        k85_config_save();
    }
    return next;
}

void k85_cpu_freq_apply_saved(void) {
    int mhz = normalize(g_config.cpu_freq_mhz);
    apply_freq(mhz);
}