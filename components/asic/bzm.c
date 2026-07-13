#include "bzm.h"

#include <string.h>

#include "utils.h"

bool bzm_work_build(const asic_work_t *source, uint16_t engine_id,
                    uint8_t logical_sequence, uint8_t timestamp_count,
                    uint8_t lead_zeros, bool enhanced_mode,
                    bzm_work_t *work)
{
    if (source == NULL || source->template == NULL || work == NULL ||
        source->handle == ASIC_WORK_HANDLE_INVALID ||
        engine_id >= BZM_MAX_ENGINE_COUNT || timestamp_count > 0x7f) {
        return false;
    }

    const mining_template_t *template = source->template;
    memset(work, 0, sizeof(*work));
    work->source = *source;
    work->engine_id = engine_id;
    work->timestamp_count = timestamp_count;
    work->starting_nonce = template->starting_nonce;
    work->end_nonce = UINT32_MAX;
    work->start_ntime = template->ntime;
    work->target = template->target;
    work->logical_sequence = logical_sequence;
    work->lead_zeros = lead_zeros;

    uint8_t header_prev_hash[32];
    uint8_t header_merkle_root[32];
    reverse_32bit_words(template->prev_block_hash, header_prev_hash);
    reverse_32bit_words(template->merkle_root, header_merkle_root);
    memcpy(&work->merkle_residue, header_merkle_root + 28,
           sizeof(work->merkle_residue));

    uint8_t midstate_data[64];
    uint8_t digest[32];
    uint32_t version = template->version;
    memcpy(midstate_data + 4, header_prev_hash, sizeof(header_prev_hash));
    memcpy(midstate_data + 36, header_merkle_root, 28);

    size_t count = enhanced_mode && template->version_mask != 0
        ? BZM_VERSION_VARIANTS : 1;
    for (size_t i = 0; i < count; ++i) {
        work->versions[i] = version;
        memcpy(midstate_data, &version, sizeof(version));
        midstate_sha256_bin(midstate_data, sizeof(midstate_data), digest);
        reverse_32bit_words(digest, work->midstates[i]);
        version = increment_bitmask(version, template->version_mask);
    }
    work->midstate_count = count;
    return true;
}

bool bzm_result_decode(const uint8_t frame[BZM_RESULT_FRAME_SIZE],
                       uint64_t timestamp_us, bzm_raw_result_t *result)
{
    if (frame == NULL || result == NULL) return false;

    uint16_t engine_status = ((uint16_t)frame[0] << 8) |
                             (uint16_t)frame[1];
    *result = (bzm_raw_result_t) {
        .engine_id = engine_status & 0x0fff,
        .status = engine_status >> 12,
        .nonce = (uint32_t)frame[2] |
                 ((uint32_t)frame[3] << 8) |
                 ((uint32_t)frame[4] << 16) |
                 ((uint32_t)frame[5] << 24),
        .sequence_id = frame[6],
        .time = frame[7],
        .timestamp_us = timestamp_us,
    };
    return result->engine_id < BZM_MAX_ENGINE_COUNT;
}

bool bzm_tdm_result_decode(
    const uint8_t frame[BZM_TDM_RESULT_FRAME_SIZE], uint64_t timestamp_us,
    bzm_raw_result_t *result)
{
    if (frame == NULL || result == NULL || frame[1] != 0x01) return false;
    if (!bzm_result_decode(frame + 2, timestamp_us, result)) return false;
    result->asic_id = frame[0];
    return true;
}

bool bzm_engine_physical_id(uint16_t logical_engine_id,
                            uint16_t *physical_engine_id)
{
    if (logical_engine_id >= BZM_ENGINES_PER_ASIC ||
        physical_engine_id == NULL) {
        return false;
    }
    uint16_t row = logical_engine_id % BZM_ENGINE_ROWS;
    uint16_t column = logical_engine_id / BZM_ENGINE_ROWS;
    *physical_engine_id = (column << 6) | row;
    return true;
}

bool bzm_engine_logical_id(uint16_t physical_engine_id,
                           uint16_t *logical_engine_id)
{
    if (logical_engine_id == NULL ||
        physical_engine_id >= BZM_MAX_ENGINE_COUNT) {
        return false;
    }
    uint16_t row = physical_engine_id & 0x3f;
    uint16_t column = physical_engine_id >> 6;
    if (row >= BZM_ENGINE_ROWS || column >= BZM_ENGINE_COLUMNS) {
        return false;
    }
    *logical_engine_id = column * BZM_ENGINE_ROWS + row;
    return true;
}

float bzm_temperature_from_code(uint16_t code)
{
    return -293.8f + (631.8f * (((float)(code & 0x0fff)) - 0.5f) /
                               4096.0f);
}
