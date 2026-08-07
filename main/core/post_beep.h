#pragma once
#include <stdint.h>

namespace k85 {

enum class PostCode : uint8_t {
    LITTLEFS_MOUNT_FAIL,
    RTC_TIME_FAIL,
    BATTERY_LOW,
    WIFI_INIT,
    BT_INIT,
};

struct PostReport {
    bool all_ok = true;
};

void k85_post_set_enabled(bool enabled);

// Немедленный бип конкретного кода (для случаев до загрузки конфига).
void k85_post_beep(PostCode code);

// Если ok == false — копит ошибку в report и бипает по коду. Если true — тихо.
void k85_post_report_check(PostReport &report, PostCode code, bool ok);

// В конце серии проверок: если всё ок — короткий успешный бип.
void k85_post_finish(PostReport &report);

} // namespace k85

