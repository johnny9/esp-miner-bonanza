#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "DS4432U.h"
#include "INA260.h"
#include "TPS546.h"
#include "adc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bzm_bridge.h"
#include "bzm_power.h"
#include "vcore.h"

#define GPIO_ASIC_ENABLE CONFIG_GPIO_ASIC_ENABLE
#define GPIO_PLUG_SENSE CONFIG_GPIO_PLUG_SENSE
#define GPIO_TPS546_PGOOD 11

static const char *TAG = "vcore";

static TPS546_CONFIG get_tps546_config(const FamilyConfig * family)
{
    TPS546_CONFIG config = {
        .TPS546_INIT_FREQUENCY = 650,
        .TPS546_INIT_PIN_DETECT_OVERRIDE = 0xFFFF,
    };

    // Set family-specific parameters
    switch (family->id) {
    case BONANZA: {
        const bzm_tps546_profile_t *profile = &BZM_TPS546_BIRDS_PROFILE;
        config.TPS546_EXTENDED_CONFIG = true;
        config.TPS546_INIT_PHASE = profile->phase;
        memcpy(config.TPS546_INIT_SMBALERT_MASK, profile->smbalert_mask,
               sizeof(config.TPS546_INIT_SMBALERT_MASK));
        config.TPS546_INIT_FREQUENCY = profile->frequency_switch_khz;
        config.TPS546_INIT_VIN_ON = profile->vin_on;
        config.TPS546_INIT_VIN_OFF = profile->vin_off;
        config.TPS546_INIT_VIN_UV_WARN_LIMIT = profile->vin_uv_warn_limit;
        config.TPS546_INIT_VIN_OV_FAULT_LIMIT = profile->vin_ov_fault_limit;
        config.TPS546_INIT_SCALE_LOOP = profile->vout_scale_loop;
        config.TPS546_INIT_VOUT_MIN = profile->vout_min;
        config.TPS546_INIT_VOUT_MAX = profile->vout_max;
        config.TPS546_INIT_VOUT_COMMAND = profile->vout_command;
        config.TPS546_INIT_IOUT_OC_WARN_LIMIT = profile->iout_oc_warn_limit;
        config.TPS546_INIT_IOUT_OC_FAULT_LIMIT = profile->iout_oc_fault_limit;
        config.TPS546_INIT_STACK_CONFIG = profile->stack_config;
        config.TPS546_INIT_SYNC_CONFIG = profile->sync_config;
        config.TPS546_INIT_INTERLEAVE = profile->interleave;
        config.TPS546_INIT_MISC_OPTIONS = profile->misc_options;
        config.TPS546_INIT_PIN_DETECT_OVERRIDE =
            profile->pin_detect_override;
        memcpy(config.TPS546_INIT_COMPENSATION_CONFIG,
               profile->compensation_config,
               sizeof(config.TPS546_INIT_COMPENSATION_CONFIG));
        config.TPS546_INIT_POWER_STAGE_CONFIG = profile->power_stage_config;
        memcpy(config.TPS546_INIT_TELEMETRY_CONFIG,
               profile->telemetry_config,
               sizeof(config.TPS546_INIT_TELEMETRY_CONFIG));
        config.TPS546_INIT_VOUT_TRIM = profile->vout_trim;
        config.TPS546_INIT_VOUT_TRANSITION_RATE =
            profile->vout_transition_rate;
        config.TPS546_INIT_IOUT_CAL_GAIN = profile->iout_cal_gain;
        config.TPS546_INIT_IOUT_CAL_OFFSET = profile->iout_cal_offset;
        config.TPS546_EXT_VOUT_MARGIN_HIGH = profile->vout_margin_high;
        config.TPS546_EXT_VOUT_MARGIN_LOW = profile->vout_margin_low;
        config.TPS546_EXT_VOUT_OV_FAULT_LIMIT =
            profile->vout_ov_fault_limit;
        config.TPS546_EXT_VOUT_OV_FAULT_RESPONSE =
            profile->vout_ov_fault_response;
        config.TPS546_EXT_VOUT_OV_WARN_LIMIT =
            profile->vout_ov_warn_limit;
        config.TPS546_EXT_VOUT_UV_WARN_LIMIT =
            profile->vout_uv_warn_limit;
        config.TPS546_EXT_VOUT_UV_FAULT_LIMIT =
            profile->vout_uv_fault_limit;
        config.TPS546_EXT_VOUT_UV_FAULT_RESPONSE =
            profile->vout_uv_fault_response;
        config.TPS546_EXT_IOUT_OC_FAULT_RESPONSE =
            profile->iout_oc_fault_response;
        config.TPS546_EXT_OT_FAULT_LIMIT = profile->ot_fault_limit;
        config.TPS546_EXT_OT_FAULT_RESPONSE = profile->ot_fault_response;
        config.TPS546_EXT_OT_WARN_LIMIT = profile->ot_warn_limit;
        config.TPS546_EXT_VIN_OV_FAULT_RESPONSE =
            profile->vin_ov_fault_response;
        config.TPS546_EXT_TON_DELAY = profile->ton_delay;
        config.TPS546_EXT_TON_RISE = profile->ton_rise;
        config.TPS546_EXT_TON_MAX_FAULT_LIMIT =
            profile->ton_max_fault_limit;
        config.TPS546_EXT_TON_MAX_FAULT_RESPONSE =
            profile->ton_max_fault_response;
        config.TPS546_EXT_TOFF_DELAY = profile->toff_delay;
        config.TPS546_EXT_TOFF_FALL = profile->toff_fall;
        break;
    }

    case GAMMA_TURBO:
        config.TPS546_INIT_PHASE = TPS546_INIT_PHASE_MULTI;
        config.TPS546_INIT_VIN_ON = 11.0;
        config.TPS546_INIT_VIN_OFF = 10.5;
        config.TPS546_INIT_VIN_UV_WARN_LIMIT = 11.0;
        config.TPS546_INIT_VIN_OV_FAULT_LIMIT = 14.0;
        config.TPS546_INIT_SCALE_LOOP = 0.25;
        config.TPS546_INIT_VOUT_MIN = 1;
        config.TPS546_INIT_VOUT_MAX = 3;
        config.TPS546_INIT_VOUT_COMMAND = 1.2;
        config.TPS546_INIT_IOUT_OC_WARN_LIMIT = 50.00;
        config.TPS546_INIT_IOUT_OC_FAULT_LIMIT = 55.00;
        // Multi-phase stacking configuration for 2 TPS modules
        config.TPS546_INIT_STACK_CONFIG = 0x0001; // 2 modules (One-Slave, 2-phase)
        config.TPS546_INIT_SYNC_CONFIG = 0xD0;    // Enable Auto Detect SYNC
        config.TPS546_INIT_COMPENSATION_CONFIG[0] = 0x12;
        config.TPS546_INIT_COMPENSATION_CONFIG[1] = 0x34;
        config.TPS546_INIT_COMPENSATION_CONFIG[2] = 0x42;
        config.TPS546_INIT_COMPENSATION_CONFIG[3] = 0x21;
        config.TPS546_INIT_COMPENSATION_CONFIG[4] = 0x04;
        break;

    case HEX:
    case SUPRA_HEX:
        config.TPS546_INIT_PHASE = TPS546_INIT_PHASE_SINGLE;
        config.TPS546_INIT_VIN_ON = 11.5;
        config.TPS546_INIT_VIN_OFF = 11.0;
        config.TPS546_INIT_VIN_UV_WARN_LIMIT = 11.0;
        config.TPS546_INIT_VIN_OV_FAULT_LIMIT = 14.0;
        config.TPS546_INIT_SCALE_LOOP = 0.125;
        config.TPS546_INIT_VOUT_MIN = 2.5;
        config.TPS546_INIT_VOUT_MAX = 4.5;
        config.TPS546_INIT_VOUT_COMMAND = 3.6;
        config.TPS546_INIT_IOUT_OC_WARN_LIMIT = 25.00;
        config.TPS546_INIT_IOUT_OC_FAULT_LIMIT = 30.00;
        // Single-phase configuration
        config.TPS546_INIT_STACK_CONFIG = 0x0000; // 1 module
        config.TPS546_INIT_SYNC_CONFIG = 0x10;    // Disable SYNC
        break;

    default: // MAX, ULTRA, SUPRA, GAMMA
        config.TPS546_INIT_PHASE = TPS546_INIT_PHASE_SINGLE;
        config.TPS546_INIT_VIN_ON = 4.8;
        config.TPS546_INIT_VIN_OFF = 4.5;
        config.TPS546_INIT_VIN_UV_WARN_LIMIT = 0;
        config.TPS546_INIT_VIN_OV_FAULT_LIMIT = 6.5;
        config.TPS546_INIT_SCALE_LOOP = 0.25;
        config.TPS546_INIT_VOUT_MIN = 1;
        config.TPS546_INIT_VOUT_MAX = 2;
        config.TPS546_INIT_VOUT_COMMAND = 1.2;
        config.TPS546_INIT_IOUT_OC_WARN_LIMIT = 25.00;
        config.TPS546_INIT_IOUT_OC_FAULT_LIMIT = 30.00;
        // Single-phase configuration
        config.TPS546_INIT_STACK_CONFIG = 0x0000; // 1 module
        config.TPS546_INIT_SYNC_CONFIG = 0x10;    // Disable SYNC
        break;
    }

    return config;
}

esp_err_t VCORE_init(GlobalState * GLOBAL_STATE)
{
    ESP_RETURN_ON_FALSE(GLOBAL_STATE->DEVICE_CONFIG.family.voltage_domains != 0, ESP_FAIL, TAG, "voltage_domains not defined");

    if (GLOBAL_STATE->DEVICE_CONFIG.bonanza_bridge) {
        gpio_config_t enable = {
            .pin_bit_mask = 1ULL << GPIO_ASIC_ENABLE,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&enable), TAG,
                            "TPS546 enable GPIO config failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_ASIC_ENABLE, 0), TAG,
                            "TPS546 enable safe-state failed");
        gpio_config_t pgood = {
            .pin_bit_mask = 1ULL << GPIO_TPS546_PGOOD,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&pgood), TAG,
                            "TPS546 PGOOD GPIO config failed");
        ESP_RETURN_ON_ERROR(BZM_bridge_init(), TAG,
                            "Bonanza bridge init failed");
        ESP_RETURN_ON_ERROR(BZM_bridge_set_5v_enabled(false), TAG,
                            "Bonanza 5V safe-state failed");
    }

    if (GLOBAL_STATE->DEVICE_CONFIG.DS4432U) {
        ESP_RETURN_ON_ERROR(DS4432U_init(), TAG, "DS4432 init failed!");
    }
    if (GLOBAL_STATE->DEVICE_CONFIG.INA260) {
        ESP_RETURN_ON_ERROR(INA260_init(), TAG, "INA260 init failed!");
    }
    if (GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        TPS546_CONFIG tps_config = get_tps546_config(&GLOBAL_STATE->DEVICE_CONFIG.family);
        ESP_RETURN_ON_ERROR(TPS546_init(tps_config), TAG, "TPS546 init failed!");
    }

    if (GLOBAL_STATE->DEVICE_CONFIG.plug_sense) {
        gpio_config_t barrel_jack_conf = {
            .pin_bit_mask = (1ULL << GPIO_PLUG_SENSE),
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&barrel_jack_conf);
        int barrel_jack_plugged_in = gpio_get_level(GPIO_PLUG_SENSE);

        gpio_set_direction(GPIO_ASIC_ENABLE, GPIO_MODE_OUTPUT);
        if (barrel_jack_plugged_in == 1 || GLOBAL_STATE->DEVICE_CONFIG.asic_enable) {
            gpio_set_level(GPIO_ASIC_ENABLE, 0);
        } else {
            // turn ASIC off
            gpio_set_level(GPIO_ASIC_ENABLE, 1);
        }
    }

    return ESP_OK;
}

static esp_err_t bonanza_set_5v_enabled(void *context, bool enabled)
{
    (void)context;
    return BZM_bridge_set_5v_enabled(enabled);
}

static esp_err_t bonanza_set_regulator_enabled(void *context, bool enabled)
{
    (void)context;
    return gpio_set_level(GPIO_ASIC_ENABLE, enabled ? 1 : 0);
}

static esp_err_t bonanza_set_vout(void *context, float volts)
{
    GlobalState *state = context;
    return state->DEVICE_CONFIG.TPS546 ? TPS546_set_vout(volts)
                                       : ESP_ERR_INVALID_STATE;
}

static esp_err_t bonanza_validate_power(void *context)
{
    (void)context;
    if (gpio_get_level(GPIO_TPS546_PGOOD) == 0) {
        ESP_LOGE(TAG, "TPS546 PGOOD remained low");
        return ESP_FAIL;
    }

    TPS546_StatusSnapshot snapshot;
    ESP_RETURN_ON_ERROR(TPS546_snapshot_status(&snapshot), TAG,
                        "TPS546 status/telemetry read failed");
    if ((snapshot.status_word &
         (TPS546_STATUS_VOUT | TPS546_STATUS_IOUT |
          TPS546_STATUS_INPUT | TPS546_STATUS_PGOOD |
          TPS546_STATUS_OFF | TPS546_STATUS_TEMP |
          TPS546_STATUS_CML)) != 0 ||
        (snapshot.operation & 0x80) == 0 ||
        snapshot.read_vin < BZM_TPS546_BIRDS_PROFILE.vin_off ||
        snapshot.read_vout < 2.65f || snapshot.read_vout > 2.95f ||
        snapshot.read_temp1 >= BZM_TPS546_BIRDS_PROFILE.ot_fault_limit) {
        ESP_LOGE(TAG, "TPS546 status/telemetry validation failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void bonanza_power_delay(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static const bzm_power_ops_t BONANZA_POWER_OPS = {
    .set_5v_enabled = bonanza_set_5v_enabled,
    .set_regulator_enabled = bonanza_set_regulator_enabled,
    .set_vout = bonanza_set_vout,
    .validate_power = bonanza_validate_power,
    .delay_ms = bonanza_power_delay,
};

esp_err_t VCORE_set_voltage(GlobalState * GLOBAL_STATE, float core_voltage)
{
    ESP_LOGI(TAG, "Set ASIC voltage = %.3fV", core_voltage);

    if (GLOBAL_STATE->DEVICE_CONFIG.bonanza_bridge) {
        if (core_voltage != 0.0f && fabsf(core_voltage - 2.8f) > 0.001f) {
            ESP_LOGE(TAG, "Bitaxe 1002 TPS rail is fixed at 2.800V");
            return ESP_ERR_INVALID_ARG;
        }

        esp_err_t err = bzm_power_set_enabled(
            &BONANZA_POWER_OPS, GLOBAL_STATE, core_voltage != 0.0f);
        if (err != ESP_OK && core_voltage != 0.0f) {
            GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg,
                     sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg),
                     "Bonanza power-up validation failed");
        }
        return err;
    }

    // Enable/disable the ASIC power enable GPIO before touching the regulator
    if (GLOBAL_STATE->DEVICE_CONFIG.asic_enable) {
        gpio_set_level(GPIO_ASIC_ENABLE, core_voltage == 0.0f ? 1 : 0);
    }

    if (GLOBAL_STATE->DEVICE_CONFIG.DS4432U) {
        if (core_voltage != 0.0f) {
            ESP_RETURN_ON_ERROR(DS4432U_set_voltage(core_voltage), TAG, "DS4432U set voltage failed!");
        }
    }
    if (GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        uint16_t voltage_domains = GLOBAL_STATE->DEVICE_CONFIG.family.voltage_domains;
        ESP_RETURN_ON_ERROR(TPS546_set_vout(core_voltage * voltage_domains), TAG, "TPS546 set voltage failed!");
    }

    return ESP_OK;
}

int16_t VCORE_get_voltage_mv(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        return TPS546_get_vout() / GLOBAL_STATE->DEVICE_CONFIG.family.voltage_domains * 1000;
    }
    return ADC_get_vcore();
}

esp_err_t VCORE_check_fault(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE->DEVICE_CONFIG.bonanza_bridge) {
        bool tripped = false;
        esp_err_t err = BZM_bridge_get_asic_trip(&tripped);
        if (err != ESP_OK) {
            GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg,
                     sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg),
                     "Bonanza control bridge unavailable");
            bzm_power_set_enabled(&BONANZA_POWER_OPS, GLOBAL_STATE, false);
            ESP_RETURN_ON_ERROR(err, TAG, "Bonanza ASIC trip read failed");
        }
        if (tripped) {
            GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg,
                     sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg),
                     "Bonanza ASIC trip asserted");
            bzm_power_set_enabled(&BONANZA_POWER_OPS, GLOBAL_STATE, false);
            return ESP_FAIL;
        }
    }
    if (GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        esp_err_t err = TPS546_check_status(GLOBAL_STATE);
        if (err != ESP_OK) {
            if (GLOBAL_STATE->DEVICE_CONFIG.bonanza_bridge) {
                GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg,
                         sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg),
                         "Bonanza TPS546 telemetry unavailable");
                bzm_power_set_enabled(&BONANZA_POWER_OPS, GLOBAL_STATE,
                                      false);
            }
            ESP_RETURN_ON_ERROR(err, TAG, "TPS546 check status failed!");
        }
        if (GLOBAL_STATE->DEVICE_CONFIG.bonanza_bridge &&
            GLOBAL_STATE->SYSTEM_MODULE.power_fault) {
            GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg,
                     sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg),
                     "Bonanza TPS546 fault asserted");
            bzm_power_set_enabled(&BONANZA_POWER_OPS, GLOBAL_STATE, false);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

const char * VCORE_get_fault_string(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        return TPS546_get_error_message();
    }
    return NULL;
}
