#include "bzm_data_link.h"
#include "bzm_transport.h"

#include <string.h>

#include "unity.h"

static void write_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static size_t encode_frame(uint16_t sequence, uint32_t pio_overflows,
                           uint32_t ring_overflows, const uint16_t *words,
                           size_t word_count, uint8_t *output)
{
    memcpy(output, "BZM2", 4);
    output[4] = 1;
    output[5] = 1;
    write_le16(output + 6, sequence);
    write_le16(output + 8, (uint16_t)word_count);
    write_le32(output + 10, pio_overflows);
    write_le32(output + 14, ring_overflows);
    for (size_t index = 0; index < word_count; ++index) {
        output[BZM_DATA_LINK_HEADER_LENGTH + index * 2U] =
            (uint8_t)words[index];
        output[BZM_DATA_LINK_HEADER_LENGTH + index * 2U + 1U] =
            (uint8_t)((words[index] >> 8) & 1U);
    }
    size_t crc_offset = BZM_DATA_LINK_HEADER_LENGTH + word_count * 2U;
    write_le32(output + crc_offset,
               bzm_data_link_crc32(output, crc_offset));
    return crc_offset + BZM_DATA_LINK_CRC_LENGTH;
}

static void read_and_expect(bzm_data_link_decoder_t *decoder,
                            const uint16_t *words, size_t word_count)
{
    uint8_t data[BZM_DATA_LINK_MAX_WORDS] = {0};
    uint8_t ninth[BZM_DATA_LINK_MAX_WORDS] = {0};
    TEST_ASSERT_EQUAL_UINT32(word_count, bzm_data_link_decoder_read(
        decoder, data, ninth, sizeof(data)));
    for (size_t index = 0; index < word_count; ++index) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)words[index], data[index]);
        TEST_ASSERT_EQUAL_UINT8((words[index] >> 8) & 1U, ninth[index]);
    }
}

TEST_CASE("BZM protected data link preserves fragmented nine-bit words",
          "[asic][bzm][data-link]")
{
    static const uint16_t words[] = {0x10a, 0x001, 0x055, 0x1ff};
    uint8_t frame[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    size_t length = encode_frame(0x1234, 7, 9, words, 4, frame);
    bzm_data_link_decoder_t decoder;
    bzm_data_link_decoder_init(&decoder);

    for (size_t index = 0; index < length; ++index) {
        TEST_ASSERT_EQUAL_UINT32(1, bzm_data_link_decoder_feed(
            &decoder, frame + index, 1));
    }
    read_and_expect(&decoder, words, 4);
    const bzm_data_link_stats_t *stats =
        bzm_data_link_decoder_stats(&decoder);
    TEST_ASSERT_EQUAL_UINT32(1, stats->received_frames);
    TEST_ASSERT_EQUAL_UINT32(7, stats->pio_fifo_overflows);
    TEST_ASSERT_EQUAL_UINT32(9, stats->software_ring_overflows);
    TEST_ASSERT_EQUAL_UINT32(0, stats->crc_failures);
}

TEST_CASE("BZM protected data link rejects corruption and resynchronizes",
          "[asic][bzm][data-link]")
{
    static const uint16_t first_words[] = {0x10a, 0x001, 0x055};
    static const uint16_t second_words[] = {0x114, 0x00d, 0x0aa};
    uint8_t stream[BZM_DATA_LINK_MAX_FRAME_LENGTH * 2];
    size_t first_length = encode_frame(5, 0, 0, first_words, 3, stream);
    size_t second_length = encode_frame(6, 0, 0, second_words, 3,
                                        stream + first_length);
    stream[BZM_DATA_LINK_HEADER_LENGTH + 2] ^= 0x80;

    bzm_data_link_decoder_t decoder;
    bzm_data_link_decoder_init(&decoder);
    size_t offset = 0;
    while (offset < first_length + second_length &&
           !bzm_data_link_decoder_has_output(&decoder)) {
        offset += bzm_data_link_decoder_feed(
            &decoder, stream + offset, first_length + second_length - offset);
    }
    read_and_expect(&decoder, second_words, 3);
    const bzm_data_link_stats_t *stats =
        bzm_data_link_decoder_stats(&decoder);
    TEST_ASSERT_EQUAL_UINT32(1, stats->crc_failures);
    TEST_ASSERT_GREATER_THAN_UINT32(0, stats->discarded_wire_bytes);
}

TEST_CASE("BZM protected data link detects loss duplication and reordering",
          "[asic][bzm][data-link]")
{
    static const uint16_t words[] = {0x10a, 0x00f, 0x032, 0x05a, 0x042};
    uint8_t frame1[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    uint8_t frame2[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    uint8_t frame3[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    size_t length1 = encode_frame(10, 0, 0, words, 5, frame1);
    size_t length2 = encode_frame(11, 0, 0, words, 5, frame2);
    size_t length3 = encode_frame(12, 0, 0, words, 5, frame3);
    bzm_data_link_decoder_t decoder;
    bzm_data_link_decoder_init(&decoder);

    TEST_ASSERT_EQUAL_UINT32(length1, bzm_data_link_decoder_feed(
        &decoder, frame1, length1));
    read_and_expect(&decoder, words, 5);

    /* Duplicate sequence 10 is discarded without exposing duplicate ASIC
     * words to the result parser. */
    TEST_ASSERT_EQUAL_UINT32(length1, bzm_data_link_decoder_feed(
        &decoder, frame1, length1));
    TEST_ASSERT_FALSE(bzm_data_link_decoder_has_output(&decoder));

    /* Sequence 12 before 11 records reordering/loss on both discontinuities. */
    TEST_ASSERT_EQUAL_UINT32(length3, bzm_data_link_decoder_feed(
        &decoder, frame3, length3));
    read_and_expect(&decoder, words, 5);
    TEST_ASSERT_EQUAL_UINT32(length2, bzm_data_link_decoder_feed(
        &decoder, frame2, length2));
    read_and_expect(&decoder, words, 5);

    const bzm_data_link_stats_t *stats =
        bzm_data_link_decoder_stats(&decoder);
    TEST_ASSERT_EQUAL_UINT32(1, stats->duplicate_frames);
    TEST_ASSERT_EQUAL_UINT32(2, stats->sequence_gaps);
}

TEST_CASE("BZM protected data link recovers after injected byte loss",
          "[asic][bzm][data-link]")
{
    static const uint16_t words[] = {0x10a, 0x001, 0x022, 0x033};
    uint8_t first[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    uint8_t second[BZM_DATA_LINK_MAX_FRAME_LENGTH];
    size_t first_length = encode_frame(20, 0, 0, words, 4, first);
    size_t second_length = encode_frame(21, 0, 0, words, 4, second);
    uint8_t stream[BZM_DATA_LINK_MAX_FRAME_LENGTH * 2];
    size_t drop = BZM_DATA_LINK_HEADER_LENGTH + 1;
    memcpy(stream, first, drop);
    memcpy(stream + drop, first + drop + 1, first_length - drop - 1);
    memcpy(stream + first_length - 1, second, second_length);
    size_t stream_length = first_length - 1 + second_length;

    bzm_data_link_decoder_t decoder;
    bzm_data_link_decoder_init(&decoder);
    size_t offset = 0;
    while (offset < stream_length &&
           !bzm_data_link_decoder_has_output(&decoder)) {
        offset += bzm_data_link_decoder_feed(
            &decoder, stream + offset, stream_length - offset);
    }
    read_and_expect(&decoder, words, 4);
    TEST_ASSERT_EQUAL_UINT32(1,
        bzm_data_link_decoder_stats(&decoder)->crc_failures);
}

TEST_CASE("BZM data link CRC matches the bridge ISO-HDLC vector",
          "[asic][bzm][data-link]")
{
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926,
        bzm_data_link_crc32((const uint8_t *)"123456789", 9));
}

TEST_CASE("BZM ASIC address marks discard only an incomplete prior packet",
          "[asic][bzm][data-link][frame-parser]")
{
    bzm_serial_transport_t transport = {0};
    TEST_ASSERT_TRUE(bzm_serial_transport_init(&transport));

    static const uint8_t partial[] = {0x0a, BZM_FRAME_RESULT, 0x55};
    static const uint8_t partial_marks[] = {1, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(0, bzm_serial_transport_ingest_marked(
        &transport, partial, partial_marks, sizeof(partial), 1));

    static const uint8_t noop[] = {
        0x14, BZM_FRAME_NOOP, '2', 'Z', 'B',
    };
    static const uint8_t noop_marks[] = {1, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(1, bzm_serial_transport_ingest_marked(
        &transport, noop, noop_marks, sizeof(noop), 2));

    bzm_serial_parser_stats_t stats;
    TEST_ASSERT_TRUE(bzm_serial_get_parser_stats(&transport, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, stats.address_mark_realignments);
    TEST_ASSERT_EQUAL_UINT32(sizeof(partial), stats.discarded_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, stats.emitted_frames);
    bzm_serial_transport_deinit(&transport);
}
