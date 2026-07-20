#include "bzm_data_link.h"

#include <string.h>

static const uint8_t MAGIC[BZM_DATA_LINK_MAGIC_LENGTH] = {'B', 'Z', 'M', '2'};
enum {
    BZM_DATA_LINK_VERSION = 1,
    BZM_DATA_LINK_TYPE_ASIC_WORDS = 1,
};

static uint16_t read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_le32(const uint8_t *value)
{
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) |
           ((uint32_t)value[3] << 24);
}

uint32_t bzm_data_link_crc32(const uint8_t *data, size_t length)
{
    if (data == NULL && length != 0) return 0;
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void bzm_data_link_decoder_init(bzm_data_link_decoder_t *decoder)
{
    if (decoder != NULL) memset(decoder, 0, sizeof(*decoder));
}

void bzm_data_link_decoder_reset_stream(bzm_data_link_decoder_t *decoder)
{
    if (decoder == NULL) return;
    decoder->frame_length = 0;
    decoder->expected_frame_length = 0;
    decoder->output_length = 0;
    decoder->output_offset = 0;
    decoder->sequence_initialized = false;
}

static void discard_prefix(bzm_data_link_decoder_t *decoder, size_t count)
{
    if (count < decoder->frame_length) {
        memmove(decoder->frame, decoder->frame + count,
                decoder->frame_length - count);
    }
    decoder->frame_length -= count;
    decoder->expected_frame_length = 0;
}

static bool header_is_valid(const bzm_data_link_decoder_t *decoder,
                            size_t *word_count)
{
    if (memcmp(decoder->frame, MAGIC, sizeof(MAGIC)) != 0 ||
        decoder->frame[4] != BZM_DATA_LINK_VERSION ||
        decoder->frame[5] != BZM_DATA_LINK_TYPE_ASIC_WORDS) {
        return false;
    }
    *word_count = read_le16(decoder->frame + 8);
    return *word_count != 0 && *word_count <= BZM_DATA_LINK_MAX_WORDS;
}

static bool decode_complete_frame(bzm_data_link_decoder_t *decoder,
                                  size_t word_count)
{
    size_t crc_offset = decoder->expected_frame_length - BZM_DATA_LINK_CRC_LENGTH;
    uint32_t expected_crc = read_le32(decoder->frame + crc_offset);
    if (bzm_data_link_crc32(decoder->frame, crc_offset) != expected_crc) {
        decoder->stats.crc_failures++;
        return false;
    }

    for (size_t index = 0; index < word_count; ++index) {
        uint8_t ninth = decoder->frame[BZM_DATA_LINK_HEADER_LENGTH + index * 2U + 1U];
        if (ninth > 1) {
            decoder->stats.invalid_frames++;
            return false;
        }
    }

    uint16_t sequence = read_le16(decoder->frame + 6);
    if (decoder->sequence_initialized) {
        if (sequence == decoder->last_sequence) {
            decoder->stats.duplicate_frames++;
            return true;
        }
        if (sequence != (uint16_t)(decoder->last_sequence + 1U)) {
            decoder->stats.sequence_gaps++;
        }
    }
    decoder->sequence_initialized = true;
    decoder->last_sequence = sequence;
    decoder->stats.pio_fifo_overflows = read_le32(decoder->frame + 10);
    decoder->stats.software_ring_overflows = read_le32(decoder->frame + 14);

    for (size_t index = 0; index < word_count; ++index) {
        decoder->output[index] =
            decoder->frame[BZM_DATA_LINK_HEADER_LENGTH + index * 2U];
        decoder->output_ninth_bits[index] =
            decoder->frame[BZM_DATA_LINK_HEADER_LENGTH + index * 2U + 1U];
    }
    decoder->output_length = word_count;
    decoder->output_offset = 0;
    decoder->stats.received_frames++;
    return true;
}

static void parse_candidate(bzm_data_link_decoder_t *decoder)
{
    while (decoder->output_length == 0) {
        while (decoder->frame_length >= sizeof(MAGIC) &&
               memcmp(decoder->frame, MAGIC, sizeof(MAGIC)) != 0) {
            discard_prefix(decoder, 1);
            decoder->stats.discarded_wire_bytes++;
        }
        if (decoder->frame_length < BZM_DATA_LINK_HEADER_LENGTH) return;

        size_t word_count = 0;
        if (!header_is_valid(decoder, &word_count)) {
            discard_prefix(decoder, 1);
            decoder->stats.discarded_wire_bytes++;
            decoder->stats.invalid_frames++;
            continue;
        }

        decoder->expected_frame_length = BZM_DATA_LINK_HEADER_LENGTH +
            word_count * 2U + BZM_DATA_LINK_CRC_LENGTH;
        if (decoder->frame_length < decoder->expected_frame_length) return;

        size_t complete_length = decoder->expected_frame_length;
        bool decoded = decode_complete_frame(decoder, word_count);
        if (!decoded) {
            discard_prefix(decoder, 1);
            decoder->stats.discarded_wire_bytes++;
            continue;
        }

        /* A duplicate has no output but is still a structurally valid frame. */
        discard_prefix(decoder, complete_length);
        if (decoder->output_length != 0) return;
    }
}

size_t bzm_data_link_decoder_feed(bzm_data_link_decoder_t *decoder,
                                  const uint8_t *input, size_t input_length)
{
    if (decoder == NULL || (input == NULL && input_length != 0)) return 0;
    size_t consumed = 0;
    while (consumed < input_length && decoder->output_length == 0) {
        if (decoder->frame_length == sizeof(decoder->frame)) {
            discard_prefix(decoder, 1);
            decoder->stats.discarded_wire_bytes++;
            decoder->stats.invalid_frames++;
        }
        decoder->frame[decoder->frame_length++] = input[consumed++];
        parse_candidate(decoder);
    }
    return consumed;
}

size_t bzm_data_link_decoder_read(bzm_data_link_decoder_t *decoder,
                                  uint8_t *output, uint8_t *ninth_bits,
                                  size_t output_capacity)
{
    if (decoder == NULL || output == NULL || output_capacity == 0 ||
        decoder->output_offset >= decoder->output_length) {
        return 0;
    }
    size_t available = decoder->output_length - decoder->output_offset;
    size_t count = available < output_capacity ? available : output_capacity;
    memcpy(output, decoder->output + decoder->output_offset, count);
    if (ninth_bits != NULL) {
        memcpy(ninth_bits, decoder->output_ninth_bits + decoder->output_offset,
               count);
    }
    decoder->output_offset += count;
    if (decoder->output_offset == decoder->output_length) {
        decoder->output_offset = 0;
        decoder->output_length = 0;
    }
    return count;
}

bool bzm_data_link_decoder_has_output(const bzm_data_link_decoder_t *decoder)
{
    return decoder != NULL && decoder->output_offset < decoder->output_length;
}

const bzm_data_link_stats_t *bzm_data_link_decoder_stats(
    const bzm_data_link_decoder_t *decoder)
{
    return decoder == NULL ? NULL : &decoder->stats;
}
