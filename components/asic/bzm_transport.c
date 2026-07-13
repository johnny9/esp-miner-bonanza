#include "bzm_transport.h"

#include <string.h>

#include "esp_timer.h"
#include "serial.h"

enum {
    BZM_REG_MIDSTATE = 0x10,
    BZM_REG_MERKLE_RESIDUE = 0x30,
    BZM_REG_START_TIMESTAMP = 0x34,
    BZM_REG_SEQUENCE_ID = 0x38,
    BZM_REG_JOB_CONTROL = 0x39,
    BZM_REG_START_NONCE = 0x3c,
    BZM_REG_END_NONCE = 0x40,
    BZM_REG_TARGET = 0x44,
    BZM_REG_TIMESTAMP_COUNT = 0x48,
    BZM_REG_ZEROS_TO_FIND = 0x49,
};

static bool append_word(uint16_t word, uint8_t *encoded, size_t capacity,
                        size_t *offset)
{
    if (*offset + 2 > capacity) return false;
    encoded[(*offset)++] = word & 0xff;
    encoded[(*offset)++] = (word >> 8) & 0x01;
    return true;
}

size_t bzm_transport_encode_write(uint8_t asic_id, uint16_t engine_id,
                                  uint8_t offset, const uint8_t *data,
                                  size_t data_len, uint8_t *encoded,
                                  size_t encoded_capacity)
{
    if (data == NULL || data_len == 0 || data_len > 256 ||
        encoded == NULL || engine_id >= BZM_MAX_ENGINE_COUNT) {
        return 0;
    }

    size_t cursor = 0;
    if (!append_word(0x100 | asic_id, encoded, encoded_capacity, &cursor) ||
        !append_word(0x020 | ((engine_id >> 8) & 0x0f), encoded,
                     encoded_capacity, &cursor) ||
        !append_word(engine_id & 0xff, encoded, encoded_capacity, &cursor) ||
        !append_word(offset, encoded, encoded_capacity, &cursor) ||
        !append_word(data_len - 1, encoded, encoded_capacity, &cursor)) {
        return 0;
    }
    for (size_t i = 0; i < data_len; ++i) {
        if (!append_word(data[i], encoded, encoded_capacity, &cursor)) {
            return 0;
        }
    }
    return append_word(0, encoded, encoded_capacity, &cursor) ? cursor : 0;
}

bool bzm_serial_write_register(bzm_serial_transport_t *transport,
                               uint16_t engine_id, uint8_t offset,
                               const void *data, size_t data_len)
{
    if (transport == NULL) return false;
    uint8_t encoded[(32 + 6) * 2];
    if (data_len > 32) return false;
    size_t length = bzm_transport_encode_write(
        transport->asic_id, engine_id, offset, data, data_len,
        encoded, sizeof(encoded));
    return length != 0 && SERIAL_send(encoded, length, false) == length;
}

bool bzm_transport_program_work(const bzm_work_t *work,
                                bzm_register_writer_t writer,
                                void *writer_context)
{
    if (work == NULL || writer == NULL ||
        work->midstate_count == 0 ||
        work->midstate_count > BZM_VERSION_VARIANTS) {
        return false;
    }

    uint8_t zero_count = work->lead_zeros > 32 ? work->lead_zeros - 32 : 0;
    if (!writer(writer_context, work->engine_id, BZM_REG_ZEROS_TO_FIND,
                &zero_count, 1) ||
        !writer(writer_context, work->engine_id, BZM_REG_TIMESTAMP_COUNT,
                &work->timestamp_count, 1) ||
        !writer(writer_context, work->engine_id, BZM_REG_START_NONCE,
                &work->starting_nonce, 4) ||
        !writer(writer_context, work->engine_id, BZM_REG_END_NONCE,
                &work->end_nonce, 4) ||
        !writer(writer_context, work->engine_id, BZM_REG_MERKLE_RESIDUE,
                &work->merkle_residue, 4) ||
        !writer(writer_context, work->engine_id, BZM_REG_START_TIMESTAMP,
                &work->start_ntime, 4) ||
        !writer(writer_context, work->engine_id, BZM_REG_TARGET,
                &work->target, 4)) {
        return false;
    }

    // The BZM engine has separate midstate and sequence FIFOs. Match the
    // reference transport by filling each FIFO in order before starting work.
    for (size_t i = 0; i < work->midstate_count; ++i) {
        if (!writer(writer_context, work->engine_id, BZM_REG_MIDSTATE,
                    work->midstates[i], 32)) {
            return false;
        }
    }
    for (size_t i = 0; i < work->midstate_count; ++i) {
        uint8_t sequence = work->midstate_count > 1
            ? (uint8_t)((work->logical_sequence << 2) | i)
            : work->logical_sequence;
        if (!writer(writer_context, work->engine_id, BZM_REG_SEQUENCE_ID,
                    &sequence, 1)) {
            return false;
        }
    }

    uint8_t job_control = 3;
    return writer(writer_context, work->engine_id, BZM_REG_JOB_CONTROL,
                  &job_control, 1);
}

static bool program_flush_job(uint16_t engine, bool enhanced_mode,
                              uint8_t job_control,
                              bzm_register_writer_t writer,
                              void *writer_context)
{
    uint8_t timestamp_count = 0xff;
    if (!writer(writer_context, engine, BZM_REG_TIMESTAMP_COUNT,
                &timestamp_count, 1)) {
        return false;
    }

    uint8_t sequence = enhanced_mode ? 0xfc : 0xff;
    size_t sequence_count = enhanced_mode ? BZM_VERSION_VARIANTS : 1;
    for (size_t i = 0; i < sequence_count; ++i) {
        uint8_t value = sequence + i;
        if (!writer(writer_context, engine, BZM_REG_SEQUENCE_ID,
                    &value, 1)) {
            return false;
        }
    }
    return writer(writer_context, engine, BZM_REG_JOB_CONTROL,
                  &job_control, 1);
}

bool bzm_transport_program_flush(uint16_t engine_count, bool enhanced_mode,
                                 bzm_register_writer_t writer,
                                 void *writer_context)
{
    if (engine_count == 0 || engine_count > BZM_ENGINES_PER_ASIC ||
        writer == NULL) {
        return false;
    }

    for (uint16_t logical_engine = 0;
         logical_engine < engine_count; ++logical_engine) {
        uint16_t engine;
        if (!bzm_engine_physical_id(logical_engine, &engine)) return false;
        if (!program_flush_job(engine, enhanced_mode, 3, writer,
                               writer_context) ||
            !program_flush_job(engine, enhanced_mode, 1, writer,
                               writer_context)) {
            return false;
        }
    }
    return true;
}

static bool serial_register_writer(void *context, uint16_t engine_id,
                                   uint8_t offset, const void *data,
                                   size_t data_len)
{
    return bzm_serial_write_register(context, engine_id, offset,
                                     data, data_len);
}

bool bzm_serial_write_work(void *context, const bzm_work_t *work)
{
    return context != NULL && bzm_transport_program_work(
        work, serial_register_writer, context);
}

bool bzm_serial_flush(void *context)
{
    bzm_serial_transport_t *transport = context;
    if (transport == NULL) return false;

    bool success = bzm_transport_program_flush(
        transport->engine_count, transport->enhanced_mode,
        serial_register_writer, transport);
    SERIAL_clear_buffer();
    return success;
}

bool bzm_serial_read_result(bzm_raw_result_t *result, uint16_t timeout_ms)
{
    static uint8_t frame[BZM_RESULT_FRAME_SIZE];
    static size_t frame_length;
    int16_t length = SERIAL_rx(frame + frame_length,
                               sizeof(frame) - frame_length,
                               timeout_ms);
    if (length < 0) {
        frame_length = 0;
        return false;
    }
    frame_length += length;
    if (frame_length != sizeof(frame)) return false;

    frame_length = 0;
    return bzm_result_decode(frame, esp_timer_get_time(), result);
}

const bzm_transport_ops_t BZM_SERIAL_TRANSPORT_OPS = {
    .write_work = bzm_serial_write_work,
    .flush = bzm_serial_flush,
};
