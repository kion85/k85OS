#pragma once

#define K85_DEVICE_NAME_COUNT 10
extern const char *const k85_device_names[K85_DEVICE_NAME_COUNT];

// Берёт индекс из g_config.device_name_idx (с защитой от выхода за границы)
const char *k85_get_device_name(void);