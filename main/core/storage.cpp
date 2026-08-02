#include "core/storage.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char* TAG = "storage";
static bool s_mounted = false;

namespace storage {

bool mount() {
    if (s_mounted) return true;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kMountPoint;
    conf.partition_label = kPartitionLabel;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Partition '%s' not found. Check partitions.csv",
                     kPartitionLabel);
        } else {
            ESP_LOGE(TAG, "Failed to init LittleFS (%s)",
                     esp_err_to_name(ret));
        }
        return false;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(kPartitionLabel, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %d/%d bytes used",
                 kMountPoint, (int)used, (int)total);
    } else {
        ESP_LOGW(TAG, "LittleFS mounted, but failed to get info (%s)",
                 esp_err_to_name(ret));
    }

    s_mounted = true;
    return true;
}

void unmount() {
    if (!s_mounted) return;
    esp_vfs_littlefs_unregister(kPartitionLabel);
    s_mounted = false;
}

bool get_info(size_t* out_total_bytes, size_t* out_used_bytes) {
    if (!s_mounted) return false;
    esp_err_t ret = esp_littlefs_info(kPartitionLabel, out_total_bytes,
                                       out_used_bytes);
    return ret == ESP_OK;
}

}  // namespace storage