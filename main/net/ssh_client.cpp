// K85_SSH_REMOVED_STUB - SSH client removed
#include "ssh_client.h"
#include "common.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void k85_run_ssh_client(void) {
    k85_show_message("SSH removed\nA+B=back");
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}