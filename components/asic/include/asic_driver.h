#ifndef ASIC_DRIVER_H
#define ASIC_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asic_result.h"
#include "mining.h"

typedef struct GlobalState GlobalState;

#define ASIC_DRIVER_HEALTH_VERSION_LENGTH 64U
#define ASIC_DRIVER_HEALTH_DETAIL_LENGTH 160U
#define ASIC_DRIVER_HEALTH_ACTION_LENGTH 96U

typedef enum {
    ASIC_DRIVER_SAFE_OFF = 0,
    ASIC_DRIVER_STARTING,
    ASIC_DRIVER_MINING,
    ASIC_DRIVER_FAULT,
    ASIC_DRIVER_MAINTENANCE,
} asic_driver_lifecycle_t;

typedef struct {
    bool available;
    asic_driver_lifecycle_t lifecycle;
    uint64_t state_since_ms;

    uint16_t asic_count;
    uint16_t expected_asic_count;
    uint16_t active_engine_count;
    uint16_t expected_engine_count;
    uint16_t fixed_frequency_mhz;
    uint16_t fixed_voltage_mv;
    float measured_voltage_v;
    float board_temperature_c;
    uint8_t fan_percent;
    uint16_t fan_rpm;

    char bridge_version[ASIC_DRIVER_HEALTH_VERSION_LENGTH];
    uint8_t bridge_protocol_major;
    uint8_t bridge_protocol_minor;
    bool bridge_compatible;

    uint64_t parser_discarded_bytes;
    uint64_t parser_recoveries;
    uint64_t address_mark_realignments;
    uint64_t transport_crc_failures;
    uint64_t transport_sequence_gaps;
    uint64_t transport_duplicate_frames;
    uint64_t transport_discarded_wire_bytes;
    uint64_t bridge_pio_fifo_overflows;
    uint64_t bridge_software_ring_overflows;
    uint64_t mapped_results;
    uint64_t locally_valid_results;
    uint64_t mapping_rejections;
    uint64_t local_rejections;
    uint64_t duplicate_results;
    uint64_t dispatch_failures;

    uint32_t last_fault_code;
    char last_fault[ASIC_DRIVER_HEALTH_DETAIL_LENGTH];
    bool automatic_retry;
    bool user_action_required;
    char recommended_action[ASIC_DRIVER_HEALTH_ACTION_LENGTH];
} asic_driver_health_t;

typedef struct {
    uint8_t (*init)(GlobalState *state);
    asic_event_t *(*process_work)(GlobalState *state);
    int (*set_max_baud)(void);
    bool (*send_work)(GlobalState *state,
                      const mining_template_t *template);
    bool (*clear_work)(GlobalState *state);
    void (*set_version_mask)(uint32_t mask);
    float (*set_hash_frequency)(float frequency);
    void (*set_nonce_space)(double nonce_percent, float frequency,
                            uint16_t asic_count, uint16_t cores);
    void (*read_registers)(void);
    float (*read_temperature)(GlobalState *state);
    void (*record_local_result)(GlobalState *state, bool valid,
                                double nonce_difficulty);
    bool (*health_snapshot)(GlobalState *state,
                            asic_driver_health_t *health);
} asic_driver_ops_t;

typedef struct {
    int id;
    uint16_t chip_id;
    const char *name;
    asic_driver_ops_t ops;
} asic_driver_t;

const asic_driver_t *asic_driver_for_id(int id);
size_t asic_driver_count(void);
const asic_driver_t *asic_driver_at(size_t index);
const char *asic_driver_lifecycle_name(asic_driver_lifecycle_t lifecycle);

#endif // ASIC_DRIVER_H
