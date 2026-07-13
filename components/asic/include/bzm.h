#ifndef BZM_H
#define BZM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asic_result.h"
#include "asic_work.h"

#define BZM_CHIP_ID 0xB0A0U
#define BZM_BAUD_RATE 5000000
#define BZM_RESULT_FRAME_SIZE 8
#define BZM_TDM_RESULT_FRAME_SIZE (BZM_RESULT_FRAME_SIZE + 2)
#define BZM_MAX_ACTIVE_WORK 256
#define BZM_MAX_ENGINE_COUNT 4096
#define BZM_VERSION_VARIANTS 4
#define BZM_NONCE_GAP_ENHANCED 0x28U
#define BZM_ENGINE_ROWS 20
#define BZM_ENGINE_COLUMNS 12
#define BZM_ENGINES_PER_ASIC (BZM_ENGINE_ROWS * BZM_ENGINE_COLUMNS)
#define BZM_CONTROL_ENGINE_ID 0xfff
#define BZM_FIRST_ASIC_ID 0x42
#define BZM_MAX_ASIC_COUNT 4

typedef struct {
    asic_work_t source;
    uint16_t engine_id;
    uint8_t timestamp_count;
    uint32_t starting_nonce;
    uint32_t end_nonce;
    uint8_t midstate_count;
    uint8_t midstates[BZM_VERSION_VARIANTS][32];
    uint32_t versions[BZM_VERSION_VARIANTS];
    uint32_t merkle_residue;
    uint32_t start_ntime;
    uint32_t target;
    uint8_t logical_sequence;
    uint8_t lead_zeros;
} bzm_work_t;

typedef struct {
    uint8_t asic_id;
    uint16_t engine_id;
    uint8_t status;
    uint32_t nonce;
    uint8_t sequence_id;
    uint8_t time;
    uint64_t timestamp_us;
} bzm_raw_result_t;

bool bzm_work_build(const asic_work_t *source, uint16_t engine_id,
                    uint8_t logical_sequence, uint8_t timestamp_count,
                    uint8_t lead_zeros, bool enhanced_mode,
                    bzm_work_t *work);

bool bzm_result_decode(const uint8_t frame[BZM_RESULT_FRAME_SIZE],
                       uint64_t timestamp_us, bzm_raw_result_t *result);
bool bzm_tdm_result_decode(
    const uint8_t frame[BZM_TDM_RESULT_FRAME_SIZE], uint64_t timestamp_us,
    bzm_raw_result_t *result);
bool bzm_engine_physical_id(uint16_t logical_engine_id,
                            uint16_t *physical_engine_id);
bool bzm_engine_logical_id(uint16_t physical_engine_id,
                           uint16_t *logical_engine_id);
float bzm_temperature_from_code(uint16_t code);

#endif // BZM_H
