#ifndef BZM_DATA_LINK_H
#define BZM_DATA_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BZM_DATA_LINK_MAGIC_LENGTH 4U
#define BZM_DATA_LINK_HEADER_LENGTH 18U
#define BZM_DATA_LINK_CRC_LENGTH 4U
#define BZM_DATA_LINK_MAX_WORDS 64U
#define BZM_DATA_LINK_MAX_FRAME_LENGTH \
    (BZM_DATA_LINK_HEADER_LENGTH + BZM_DATA_LINK_MAX_WORDS * 2U + BZM_DATA_LINK_CRC_LENGTH)

typedef struct {
    uint32_t crc_failures;
    uint32_t sequence_gaps;
    uint32_t duplicate_frames;
    uint32_t discarded_wire_bytes;
    uint32_t invalid_frames;
    uint32_t received_frames;
    uint32_t pio_fifo_overflows;
    uint32_t software_ring_overflows;
} bzm_data_link_stats_t;

typedef struct {
    uint8_t frame[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    size_t frame_length;
    size_t expected_frame_length;

    uint8_t output[BZM_DATA_LINK_MAX_WORDS];
    uint8_t output_ninth_bits[BZM_DATA_LINK_MAX_WORDS];
    size_t output_length;
    size_t output_offset;

    bool sequence_initialized;
    uint16_t last_sequence;
    bzm_data_link_stats_t stats;
} bzm_data_link_decoder_t;

void bzm_data_link_decoder_init(bzm_data_link_decoder_t *decoder);
/* Clear partial wire/output state and sequence history while retaining
 * cumulative diagnostic counters. */
void bzm_data_link_decoder_reset_stream(bzm_data_link_decoder_t *decoder);

/* Consume as much input as possible. Consumption stops when one decoded ASIC
 * batch is ready so the caller can preserve any remaining wire bytes. */
size_t bzm_data_link_decoder_feed(bzm_data_link_decoder_t *decoder,
                                  const uint8_t *input, size_t input_length);
size_t bzm_data_link_decoder_read(bzm_data_link_decoder_t *decoder,
                                  uint8_t *output, uint8_t *ninth_bits,
                                  size_t output_capacity);
bool bzm_data_link_decoder_has_output(const bzm_data_link_decoder_t *decoder);
const bzm_data_link_stats_t *bzm_data_link_decoder_stats(
    const bzm_data_link_decoder_t *decoder);

uint32_t bzm_data_link_crc32(const uint8_t *data, size_t length);

#endif /* BZM_DATA_LINK_H */
