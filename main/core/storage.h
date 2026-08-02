
Storage · H
#pragma once
#include <cstddef>
 
namespace storage {
 
// Точка монтирования LittleFS (аналог "/flash" в MicroPython)
constexpr const char* kMountPoint = "/littlefs";
constexpr const char* kPartitionLabel = "storage";
 
// Монтирует LittleFS. Возвращает true при успехе.
// format_if_mount_failed=true — если раздел битый/пустой, будет
// отформатирован автоматически (как первый запуск на новом устройстве).
bool mount();
 
// Отмонтировать (обычно не требуется, но полезно для factory reset).
void unmount();
 
// Инфо о разделе в байтах (для System Info: ROM used/total).
// Возвращает false, если раздел не смонтирован.
bool get_info(size_t* out_total_bytes, size_t* out_used_bytes);
 
}  // namespace storage
 
