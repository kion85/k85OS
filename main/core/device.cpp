#include "device.h"
#include "config.h"

const char *const k85_device_names[K85_DEVICE_NAME_COUNT] = {
    "k85 Core", "M5 Eclipse", "Pocket Terminal", "CyberDeck Zero", "NeonNode",
    "FluxPanel", "IronShell", "DarkDock", "SynthPad", "PixelGate",
};

const char *k85_get_device_name(void) {
    int idx = g_config.device_name_idx;
    if (idx < 0 || idx >= K85_DEVICE_NAME_COUNT) idx = 0;
    return k85_device_names[idx];
}