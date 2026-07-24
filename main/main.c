#include <stdlib.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "adc.h"
#include "asic.h"
#include "asic_init.h"
#include "asic_reset.h"
#include "asic_result_task.h"
#include "bap/bap.h"
#include "bzm_bridge_update.h"
#include "bzm_controller.h"
#include "connect.h"
#include "create_jobs_task.h"
#include "device_config.h"
#include "fan_controller_task.h"
#include "filesystem.h"
#include "hashrate_monitor_task.h"
#include "http_server.h"
#include "i2c_bitaxe.h"
#include "input.h"
#include "log_buffer.h"
#include "nvs_config.h"
#include "protocol_coordinator.h"
#include "self_test.h"
#include "serial.h"
#include "statistics_task.h"
#include "system.h"
#include "task_monitor.h"

static GlobalState GLOBAL_STATE;

static const char * TAG = "bitaxe";

static void heap_alloc_failed_hook(size_t requested_size, uint32_t caps, const char * function_name)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        ESP_EARLY_LOGE(TAG, "%s failed to allocate %zu bytes from PSRAM", function_name, requested_size);
        abort();
    }
}

static void * cjson_malloc_psram(size_t size)
{
    if (esp_psram_is_initialized()) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    return malloc(size);
}

static void cjson_free_psram(void * ptr)
{
    free(ptr);
}

void app_main(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(heap_alloc_failed_hook));

    cJSON_Hooks hooks = {.malloc_fn = cjson_malloc_psram, .free_fn = cjson_free_psram};
    cJSON_InitHooks(&hooks);
    if (esp_psram_is_initialized()) {
        GLOBAL_STATE.psram_is_available = true;
        log_buffer_init();
    } else {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
    }

    ESP_LOGI(TAG, "Welcome to the bitaxe - FOSS || GTFO!");

    if (xTaskCreateWithCaps(cpu_monitor_task, "cpu_monitor", 4096, (void *) &GLOBAL_STATE, 1, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating cpu monitor task");
    }
#ifdef CONFIG_ENABLE_TASK_MONITOR
    if (xTaskCreateWithCaps(task_monitor_task, "task_monitor", 8192, NULL, 1, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating task monitor task");
    }
#endif

    // Init I2C
    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    // wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Init ADC
    ADC_init();

    // initialize the ESP32 NVS
    if (nvs_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    // Ensure SSID is initialized before any screen/self-test uses it.
    GLOBAL_STATE.SYSTEM_MODULE.ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);
    if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
        ESP_LOGW(TAG, "No SSID configured in NVS, using empty string");
        GLOBAL_STATE.SYSTEM_MODULE.ssid = strdup("");
        if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for SSID");
            return;
        }
    }

    if (device_config_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device config");
        return;
    }

    // Board identity decides whether reset is a direct GPIO or is owned by
    // the Bonanza RP2040 bridge. Never drive GPIO1 before that decision.
    esp_err_t reset_safe_err = asic_hold_reset_low(&GLOBAL_STATE);
    if (reset_safe_err == ESP_OK) {
        ESP_LOGI(TAG, "ASIC reset initialized to the safe state");
    } else if (bzm_bridge_update_boot_recovery_allowed(
                   &GLOBAL_STATE.DEVICE_CONFIG, reset_safe_err)) {
        /*
         * A factory-blank RP2040 cannot acknowledge this command. Continue
         * booting so Wi-Fi, AxeOS, and the onboard SWD recovery endpoint stay
         * available. The Bonanza controller remains fail-closed and will not
         * energize or dispatch work without coherent bridge safety evidence.
         */
        GLOBAL_STATE.SYSTEM_MODULE.mining_paused = true;
        ESP_LOGE(TAG,
                 "Bonanza bridge unavailable during early safe-state request: "
                 "%s; continuing for HTTP bridge recovery",
                 esp_err_to_name(reset_safe_err));
    } else {
        ESP_ERROR_CHECK(reset_safe_err);
    }

    if (self_test_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init self test");
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);
    if (scoreboard_init(&GLOBAL_STATE.SYSTEM_MODULE.scoreboard) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init scoreboard");
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        wifi_init(&GLOBAL_STATE);
    }

    esp_err_t system_init_ret = SYSTEM_init_peripherals(&GLOBAL_STATE);

    if (system_init_ret == ESP_OK) {
        if (GLOBAL_STATE.DEVICE_CONFIG.bonanza_bridge) {
            /*
             * Board 1002 uses its fixed-profile production controller instead
             * of the legacy tunable voltage path.
             */
            esp_err_t runtime_err = bzm_controller_init(&GLOBAL_STATE);
            if (runtime_err != ESP_OK) {
                ESP_LOGE(TAG, "Bonanza safe-off runtime initialization failed: %s", esp_err_to_name(runtime_err));
                system_init_ret = runtime_err;
            }
        } else {
            if (xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating power management task");
            }
            if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
                if (xTaskCreate(FAN_CONTROLLER_task, "fan_controller", 8192, (void *) &GLOBAL_STATE, 5, NULL) != pdPASS) {
                    ESP_LOGE(TAG, "Error creating fan controller task");
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "Critical peripheral initialization failure (%s). Entering degraded mode.",
                 esp_err_to_name(GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret));
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        // start the API for AxeOS
        start_rest_server((void *) &GLOBAL_STATE);
    }

    // After mounting SPIFFS
    SYSTEM_init_versions(&GLOBAL_STATE);

    // UART2 is the Bonanza control link on board 1002. Other products keep
    // using it for the Bitaxe Accessory Port.
    if (!GLOBAL_STATE.DEVICE_CONFIG.bonanza_bridge) {
        esp_err_t bap_ret = BAP_init(&GLOBAL_STATE);
        if (bap_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize BAP interface: %d", bap_ret);
            // Continue anyway, as BAP is not critical for core functionality
        }
    } else {
        ESP_LOGI(TAG, "UART2 reserved for the Bonanza RP2040 bridge; BAP disabled");
    }

    while (!GLOBAL_STATE.SYSTEM_MODULE.is_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    queue_init(&GLOBAL_STATE.stratum_queue);

    if (GLOBAL_STATE.DEVICE_CONFIG.bonanza_bridge) {
        if (!bzm_controller_mining_stack_ready()) {
            ESP_LOGE(TAG, "Bonanza remained safe-off after automatic startup failure");
        }
        return;
    }

    if (system_init_ret == ESP_OK) {
        if (asic_initialize(&GLOBAL_STATE, ASIC_INIT_COLD_BOOT, 0) == 0) {
            if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
                return;
            }

            self_test_show_message(&GLOBAL_STATE, GLOBAL_STATE.SYSTEM_MODULE.asic_status);
            system_init_ret = ESP_FAIL;
        } else {
            if (xTaskCreate(create_jobs_task, "stratum miner", 8192, (void *) &GLOBAL_STATE, 20, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating stratum miner task");
            }
            if (xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating asic result task");
            }

            if (xTaskCreateWithCaps(hashrate_monitor_task, "hashrate monitor", 8192, (void *) &GLOBAL_STATE, 5, NULL,
                                    MALLOC_CAP_SPIRAM) != pdPASS) {
                ESP_LOGE(TAG, "Error creating hashrate monitor task");
            }
            if (xTaskCreateWithCaps(statistics_task, "statistics", 8192, (void *) &GLOBAL_STATE, 3, NULL, MALLOC_CAP_SPIRAM) !=
                pdPASS) {
                ESP_LOGE(TAG, "Error creating statistics task");
            }
        }
    }

    protocol_coordinator_init(&GLOBAL_STATE);
    if (xTaskCreateWithCaps(protocol_coordinator_task, "protocol coord", 3072, (void *) &GLOBAL_STATE, 5, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating protocol coordinator task");
    }

    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;
        if (xTaskCreateWithCaps(self_test_task, "self_test", 8192, (void *) &GLOBAL_STATE, 10, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Error creating self test task");
        }
    }
}
