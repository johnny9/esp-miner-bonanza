#ifndef ASIC_DRIVER_H
#define ASIC_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asic_result.h"
#include "mining.h"

typedef struct GlobalState GlobalState;

typedef struct {
    uint8_t (*init)(GlobalState *state);
    asic_event_t *(*process_work)(GlobalState *state);
    int (*set_max_baud)(void);
    bool (*send_work)(GlobalState *state,
                      const mining_template_t *template);
    void (*set_version_mask)(uint32_t mask);
    float (*set_hash_frequency)(float frequency);
    void (*set_nonce_space)(double nonce_percent, float frequency,
                            uint16_t asic_count, uint16_t cores);
    void (*read_registers)(void);
    float (*read_temperature)(GlobalState *state);
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

#endif // ASIC_DRIVER_H
