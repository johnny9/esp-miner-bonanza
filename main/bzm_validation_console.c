#include "bzm_validation_console.h"

#include <stdio.h>

#include "bzm_local_arm.h"
#include "bzm_runtime_control.h"
#include "bzm_validation_runtime.h"
#include "esp_console.h"
#include "esp_log.h"

static const char * TAG = "bzm_console";

#ifdef CONFIG_BZM_1002_USB_SERIAL_ARM
static esp_console_repl_t * REPL;

static int arm_command(int argc, char ** argv)
{
    if (argc != 2) {
        printf("BAD: use exactly: bzm-arm %s\n", BZM_RUNTIME_POWER_CONFIRMATION);
        return 1;
    }

    uint32_t remaining_ms = 0;
    bzm_local_arm_result_t result = bzm_validation_runtime_arm_local(argv[1], &remaining_ms);
    if (result != BZM_LOCAL_ARM_ACCEPTED) {
        printf("BAD: BZM arm rejected: %s; runtime must be GOOD OFF_SAFE\n", bzm_local_arm_result_name(result));
        return 1;
    }

    printf("GOOD: BZM powered validation armed once for %lu ms\n", (unsigned long) remaining_ms);
    return 0;
}
#endif

esp_err_t bzm_validation_console_init(void)
{
#ifdef CONFIG_BZM_1002_USB_SERIAL_ARM
    if (REPL != NULL)
        return ESP_OK;

    const esp_console_cmd_t command = {
        .command = "bzm-arm",
        .help = "Issue one local, short-lived powered-validation arm: "
                "bzm-arm ENERGIZE_BZM_1002",
        .hint = NULL,
        .func = arm_command,
    };
    esp_err_t result = esp_console_cmd_register(&command);
    if (result != ESP_OK)
        return result;

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "bzm>";
    repl_config.max_cmdline_length = 64;

    esp_console_dev_usb_serial_jtag_config_t device_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    result = esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &REPL);
    if (result != ESP_OK)
        return result;
    result = esp_console_start_repl(REPL);
    if (result != ESP_OK)
        return result;

    ESP_LOGI(TAG, "Local USB console arm ready: bzm-arm %s (single use, %d s)", BZM_RUNTIME_POWER_CONFIRMATION,
             CONFIG_BZM_1002_USB_SERIAL_ARM_WINDOW_SECONDS);
#endif
    return ESP_OK;
}
