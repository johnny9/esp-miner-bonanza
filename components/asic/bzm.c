#include "bzm.h"

#include <string.h>

#include "utils.h"

static size_t bzm_build_versions(uint32_t base_version,
                                 uint32_t version_mask,
                                 bool enhanced_mode,
                                 uint32_t versions[BZM_VERSION_VARIANTS])
{
    versions[0] = base_version;
    if (!enhanced_mode || version_mask == 0) {
        return 1;
    }

    /* Match BIRDS/cgminer's vmask_001[0,2,4,8] construction. The
     * least-significant non-zero hex nibble is one sub-job, the remaining
     * mask is the other, and the fourth sub-job combines both. */
    unsigned trailing_nibbles = 0;
    uint32_t shifted_mask = version_mask;
    while ((shifted_mask & 0x0fU) == 0U) {
        shifted_mask >>= 4;
        trailing_nibbles++;
    }
    uint32_t low_nibble = (shifted_mask & 0x0fU)
        << (trailing_nibbles * 4U);
    versions[1] = base_version | low_nibble;
    versions[2] = base_version | (version_mask - low_nibble);
    versions[3] = base_version | version_mask;
    return BZM_VERSION_VARIANTS;
}

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
    memcpy(midstate_data + 4, header_prev_hash, sizeof(header_prev_hash));
    memcpy(midstate_data + 36, header_merkle_root, 28);

    size_t count = bzm_build_versions(template->version,
                                      template->version_mask,
                                      enhanced_mode, work->versions);
    for (size_t i = 0; i < count; ++i) {
        uint32_t version = work->versions[i];
        memcpy(midstate_data, &version, sizeof(version));
        midstate_sha256_bin(midstate_data, sizeof(midstate_data), digest);
        /* mbedTLS exposes each SHA-256 state word as big-endian bytes.
         * BIRDS/cgminer writes native little-endian uint32_t h0..h7 words to
         * Bonanza, so swap bytes within each word while preserving the word
         * order. Bitmain-family packets instead reverse the word order. */
        reverse_endianness_per_word(digest);
        memcpy(work->midstates[i], digest, sizeof(digest));
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

bool bzm_raw_result_has_valid_nonce(const bzm_raw_result_t *result)
{
    return result != NULL && (result->status & 0x08U) != 0U;
}

bool bzm_engine_physical_id(uint16_t logical_engine_id,
                            uint16_t *physical_engine_id)
{
    bzm_engine_location_t engine;
    if (physical_engine_id == NULL ||
        !bzm_topology_engine_at(logical_engine_id, &engine)) {
        return false;
    }
    *physical_engine_id = engine.physical_id;
    return true;
}

bool bzm_engine_logical_id(uint16_t physical_engine_id,
                           uint16_t *logical_engine_id)
{
    bzm_engine_location_t engine;
    if (logical_engine_id == NULL ||
        !bzm_topology_from_physical_id(physical_engine_id, &engine)) {
        return false;
    }
    *logical_engine_id = engine.topology_index;
    return true;
}

float bzm_temperature_from_code(uint16_t code)
{
    return -293.8f + (631.8f * (((float)(code & 0x0fff)) - 0.5f) /
                               4096.0f);
}
