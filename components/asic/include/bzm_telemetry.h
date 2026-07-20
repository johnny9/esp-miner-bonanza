#ifndef BZM_TELEMETRY_H
#define BZM_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bzm_frame_parser.h"

#define BZM_CH2_CONFIRM_MAX_SAMPLES 10U

typedef struct
{
    float temperature_min_c;
    float temperature_max_c;
    float ch0_min_mv;
    float ch0_max_mv;
    float ch1_min_mv;
    float ch1_max_mv;
    /* CH2 is not a third stack rail. It is the differential between the top
     * stack VSS and bottom stack VDD and should remain near zero. */
    float ch2_abs_max_mv;
    /* Maximum allowed difference between the bottom and top stack channels. */
    float max_stack_spread_mv;
} bzm_telemetry_bounds_t;

typedef struct
{
    uint8_t asic_id;
    uint64_t timestamp_us;
    bool received;

    uint16_t temperature_code;
    float temperature_c;
    bool thermal_enabled;
    bool thermal_validity;
    bool thermal_fault;
    bool thermal_trip;
    bool thermal_valid;

    uint16_t ch0_code;
    uint16_t ch1_code;
    uint16_t ch2_code;
    float ch0_mv;
    float ch1_mv;
    float ch2_mv;
    bool voltage_enabled;
    bool voltage_fault;
    bool voltage_trip;
    bool voltage_valid;

    /* TDM byte 7 bit 7 is the combined PLL0-and-PLL1 lock indication. Bits
     * 5 and 6 are reserved by the Intel Blockscale 1000 datasheet. */
    bool pll_locked;
    bool valid;
    bool trip;
} bzm_telemetry_sample_t;

typedef struct
{
    bzm_telemetry_sample_t samples[BZM_MAX_ASIC_COUNT];
} bzm_telemetry_store_t;

typedef struct
{
    uint8_t consecutive_excursions[BZM_MAX_ASIC_COUNT];
    uint64_t last_timestamp_us[BZM_MAX_ASIC_COUNT];
} bzm_ch2_confirmation_t;

typedef struct
{
    uint8_t consecutive_anomalies[BZM_MAX_ASIC_COUNT];
    uint64_t last_timestamp_us[BZM_MAX_ASIC_COUNT];
} bzm_telemetry_confirmation_t;

typedef struct
{
    uint8_t consecutive_unlocks[BZM_MAX_ASIC_COUNT];
    uint64_t last_timestamp_us[BZM_MAX_ASIC_COUNT];
} bzm_pll_lock_confirmation_t;

typedef enum
{
    BZM_CH2_CONFIRMATION_GOOD = 0,
    BZM_CH2_CONFIRMATION_PENDING = 1,
    BZM_CH2_CONFIRMATION_CONTINUOUS = 2,
    BZM_CH2_CONFIRMATION_NO_NEW_SAMPLE = 3,
    BZM_CH2_CONFIRMATION_INVALID = 4,
} bzm_ch2_confirmation_result_t;

float bzm_telemetry_temperature_from_code(uint16_t code);
float bzm_telemetry_voltage_from_code_mv(uint16_t code);

bool bzm_telemetry_decode(uint8_t asic_id, const uint8_t * payload, size_t payload_length, uint64_t timestamp_us,
                          bzm_telemetry_sample_t * sample);

void bzm_telemetry_store_init(bzm_telemetry_store_t * store);
bool bzm_telemetry_store_apply_frame(bzm_telemetry_store_t * store, const bzm_frame_t * frame);
const bzm_telemetry_sample_t * bzm_telemetry_store_get(const bzm_telemetry_store_t * store, uint8_t asic_id);

bool bzm_telemetry_value_in_bounds(float value, float minimum, float maximum);
bool bzm_telemetry_sample_is_fresh(const bzm_telemetry_sample_t * sample, uint64_t now_us, uint64_t max_age_us);
bool bzm_telemetry_sample_has_immediate_trip(const bzm_telemetry_sample_t * sample);
bool bzm_telemetry_sample_is_safe_except_ch2(const bzm_telemetry_sample_t * sample, uint64_t now_us, uint64_t max_age_us,
                                             const bzm_telemetry_bounds_t * bounds, bool require_clock_locks);
bool bzm_telemetry_sample_is_within_bounds(const bzm_telemetry_sample_t * sample, const bzm_telemetry_bounds_t * bounds);
bool bzm_telemetry_sample_is_safe(const bzm_telemetry_sample_t * sample, uint64_t now_us, uint64_t max_age_us,
                                  const bzm_telemetry_bounds_t * bounds, bool require_clock_locks);

void bzm_ch2_confirmation_init(bzm_ch2_confirmation_t * confirmation);
bzm_ch2_confirmation_result_t bzm_ch2_confirmation_observe(bzm_ch2_confirmation_t * confirmation,
                                                           const bzm_telemetry_store_t * store,
                                                           const bzm_telemetry_bounds_t * bounds,
                                                           uint8_t required_consecutive_samples, uint8_t * culprit_asic_id,
                                                           uint8_t * observed_consecutive_samples);

void bzm_telemetry_confirmation_init(bzm_telemetry_confirmation_t * confirmation);
bzm_ch2_confirmation_result_t bzm_telemetry_confirmation_observe(
    bzm_telemetry_confirmation_t * confirmation, const bzm_telemetry_store_t * store, uint64_t now_us,
    uint64_t max_age_us, const bzm_telemetry_bounds_t * bounds, bool require_clock_locks,
    uint8_t required_consecutive_samples, uint8_t * culprit_asic_id, uint8_t * observed_consecutive_samples);

/* Qualify only the unchecksummed combined PLL0/PLL1 telemetry bit. Initial
 * direct PLL register/lock validation remains a separate hard gate. */
void bzm_pll_lock_confirmation_init(bzm_pll_lock_confirmation_t * confirmation);
bzm_ch2_confirmation_result_t bzm_pll_lock_confirmation_observe(
    bzm_pll_lock_confirmation_t * confirmation, const bzm_telemetry_store_t * store, uint64_t now_us,
    uint64_t max_age_us, uint8_t required_consecutive_samples, uint8_t * culprit_asic_id,
    uint8_t * observed_consecutive_samples);

#endif // BZM_TELEMETRY_H
