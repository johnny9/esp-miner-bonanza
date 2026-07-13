#include "bzm_transport.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    BZM_REG_ASIC_ID = 0x0b,
};

typedef struct {
    uint8_t asic_id;
} serial_register_context_t;

#define BZM_PENDING_RESULT_COUNT 32
static bzm_raw_result_t PENDING_RESULTS[BZM_PENDING_RESULT_COUNT];
static size_t PENDING_RESULT_HEAD;
static size_t PENDING_RESULT_LENGTH;

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

size_t bzm_transport_encode_read(uint8_t asic_id, uint16_t engine_id,
                                 uint8_t offset, size_t data_len,
                                 uint8_t *encoded,
                                 size_t encoded_capacity)
{
    if (data_len == 0 || data_len > 256 || encoded == NULL ||
        engine_id >= BZM_MAX_ENGINE_COUNT) {
        return 0;
    }

    size_t cursor = 0;
    return append_word(0x100 | asic_id, encoded, encoded_capacity, &cursor) &&
           append_word(0x030 | ((engine_id >> 8) & 0x0f), encoded,
                       encoded_capacity, &cursor) &&
           append_word(engine_id & 0xff, encoded, encoded_capacity, &cursor) &&
           append_word(offset, encoded, encoded_capacity, &cursor) &&
           append_word(data_len - 1, encoded, encoded_capacity, &cursor) &&
           append_word(0, encoded, encoded_capacity, &cursor)
        ? cursor : 0;
}

size_t bzm_transport_encode_noop(uint8_t asic_id, uint8_t *encoded,
                                 size_t encoded_capacity)
{
    if (encoded == NULL) return 0;
    size_t cursor = 0;
    return append_word(0x100 | asic_id, encoded, encoded_capacity, &cursor) &&
           append_word(0x0f0, encoded, encoded_capacity, &cursor)
        ? cursor : 0;
}

size_t bzm_discover_chain(size_t expected_count, uint8_t first_asic_id,
                          uint8_t *asic_ids, size_t asic_ids_capacity,
                          const bzm_chain_ops_t *ops, void *ops_context)
{
    if (expected_count == 0 || expected_count > BZM_MAX_ASIC_COUNT ||
        asic_ids == NULL || asic_ids_capacity < expected_count ||
        ops == NULL || ops->noop == NULL ||
        ops->write_register == NULL || ops->read_register == NULL) {
        return 0;
    }

    size_t detected = 0;
    for (size_t i = 0; i < expected_count; ++i) {
        uint8_t asic_id = first_asic_id + i;
        uint8_t id_config[4] = {
            asic_id,
            i == 0 ? 0x00 : 0x01,
            0x00,
            0x00,
        };
        uint8_t readback[4] = {0};

        if (!ops->noop(ops_context, BZM_BROADCAST_ASIC) ||
            !ops->write_register(ops_context, BZM_BROADCAST_ASIC,
                                 BZM_CONTROL_ENGINE_ID, BZM_REG_ASIC_ID,
                                 id_config, sizeof(id_config))) {
            break;
        }
        if (ops->delay_ms != NULL) ops->delay_ms(ops_context, 200);
        if (!ops->read_register(ops_context, asic_id,
                                BZM_CONTROL_ENGINE_ID, BZM_REG_ASIC_ID,
                                readback, sizeof(readback)) ||
            readback[0] != asic_id ||
            (readback[1] & 0x01) != (i == 0 ? 0 : 1) ||
            !ops->noop(ops_context, asic_id)) {
            break;
        }
        asic_ids[detected++] = asic_id;
    }
    return detected;
}

bool bzm_partition_nonce_range(uint32_t starting_nonce, uint32_t end_nonce,
                               size_t partition, size_t partition_count,
                               uint32_t *partition_start,
                               uint32_t *partition_end)
{
    if (partition_count == 0 || partition >= partition_count ||
        partition_start == NULL || partition_end == NULL ||
        end_nonce < starting_nonce) {
        return false;
    }
    uint64_t nonce_count = (uint64_t)end_nonce - starting_nonce + 1;
    uint64_t nonce_step = nonce_count / partition_count;
    if (nonce_step == 0) return false;
    *partition_start = starting_nonce + nonce_step * partition;
    *partition_end = partition + 1 == partition_count
        ? end_nonce
        : starting_nonce + nonce_step * (partition + 1) - 1;
    return true;
}

static bool serial_write_register_raw(uint8_t asic_id, uint16_t engine_id,
                                     uint8_t offset, const void *data,
                                     size_t data_len)
{
    uint8_t encoded[(32 + 6) * 2];
    if (data_len > 32) return false;
    size_t length = bzm_transport_encode_write(
        asic_id, engine_id, offset, data, data_len,
        encoded, sizeof(encoded));
    return length != 0 && SERIAL_send(encoded, length, false) == length;
}

bool bzm_serial_write_register(bzm_serial_transport_t *transport,
                               uint16_t engine_id, uint8_t offset,
                               const void *data, size_t data_len)
{
    if (transport == NULL || transport->asic_count == 0) return false;
    return serial_write_register_raw(transport->asic_ids[0], engine_id,
                                     offset, data, data_len);
}

bool bzm_serial_write_register_to(bzm_serial_transport_t *transport,
                                  uint8_t asic_id, uint16_t engine_id,
                                  uint8_t offset, const void *data,
                                  size_t data_len)
{
    if (transport == NULL) return false;
    return serial_write_register_raw(asic_id, engine_id, offset,
                                     data, data_len);
}

static bool serial_read_exact(uint8_t *data, size_t data_len,
                              uint16_t timeout_ms)
{
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (received < data_len) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) return false;
        uint16_t remaining_ms = (remaining_us + 999) / 1000;
        int16_t length = SERIAL_rx(data + received, data_len - received,
                                   remaining_ms);
        if (length < 0) return false;
        received += length;
    }
    return true;
}

static void queue_result(const bzm_raw_result_t *result)
{
    if (PENDING_RESULT_LENGTH == BZM_PENDING_RESULT_COUNT) {
        PENDING_RESULT_HEAD =
            (PENDING_RESULT_HEAD + 1) % BZM_PENDING_RESULT_COUNT;
        PENDING_RESULT_LENGTH--;
    }
    size_t tail = (PENDING_RESULT_HEAD + PENDING_RESULT_LENGTH) %
                  BZM_PENDING_RESULT_COUNT;
    PENDING_RESULTS[tail] = *result;
    PENDING_RESULT_LENGTH++;
}

static bool dequeue_result(bzm_raw_result_t *result)
{
    if (result == NULL || PENDING_RESULT_LENGTH == 0) return false;
    *result = PENDING_RESULTS[PENDING_RESULT_HEAD];
    PENDING_RESULT_HEAD =
        (PENDING_RESULT_HEAD + 1) % BZM_PENDING_RESULT_COUNT;
    PENDING_RESULT_LENGTH--;
    return true;
}

static bool read_tdm_header(uint8_t header[2], uint16_t timeout_ms)
{
    if (!serial_read_exact(header, 2, timeout_ms)) return false;
    while (header[0] < BZM_FIRST_ASIC_ID ||
           header[0] >= BZM_FIRST_ASIC_ID + BZM_MAX_ASIC_COUNT ||
           (header[1] != 0x01 && header[1] != 0x03 &&
            header[1] != 0x0f)) {
        header[0] = header[1];
        if (!serial_read_exact(&header[1], 1, timeout_ms)) return false;
    }
    return true;
}

static bool serial_noop(void *context, uint8_t asic_id)
{
    (void)context;
    uint8_t request[4];
    uint8_t response[8];
    size_t request_length = bzm_transport_encode_noop(
        asic_id, request, sizeof(request));
    if (request_length == 0 ||
        SERIAL_send(request, request_length, false) != request_length) {
        return false;
    }
    return SERIAL_rx(response, sizeof(response), 150) > 0;
}

static bool serial_write_register_op(void *context, uint8_t asic_id,
                                     uint16_t engine_id, uint8_t offset,
                                     const void *data, size_t data_len)
{
    (void)context;
    return serial_write_register_raw(asic_id, engine_id, offset,
                                     data, data_len);
}

static bool serial_read_register_op(void *context, uint8_t asic_id,
                                    uint16_t engine_id, uint8_t offset,
                                    void *data, size_t data_len)
{
    (void)context;
    if (data == NULL || data_len == 0 || data_len > 32) return false;
    uint8_t request[12];
    size_t request_length = bzm_transport_encode_read(
        asic_id, engine_id, offset, data_len, request, sizeof(request));
    if (request_length == 0 ||
        SERIAL_send(request, request_length, false) != request_length) {
        return false;
    }

    int64_t deadline = esp_timer_get_time() + 500000;
    while (esp_timer_get_time() < deadline) {
        uint8_t header[2];
        uint16_t remaining_ms =
            (deadline - esp_timer_get_time() + 999) / 1000;
        if (!read_tdm_header(header, remaining_ms)) return false;
        if (header[1] == 0x01) {
            uint8_t frame[BZM_TDM_RESULT_FRAME_SIZE] = {
                header[0], header[1],
            };
            bzm_raw_result_t result;
            if (!serial_read_exact(frame + 2, BZM_RESULT_FRAME_SIZE,
                                   remaining_ms) ||
                !bzm_tdm_result_decode(frame, esp_timer_get_time(),
                                       &result)) {
                return false;
            }
            queue_result(&result);
            continue;
        }
        if (header[1] == 0x03 && header[0] == asic_id) {
            return serial_read_exact(data, data_len, remaining_ms);
        }
        if (header[1] == 0x0f) {
            uint8_t noop_payload[3];
            if (!serial_read_exact(noop_payload, sizeof(noop_payload),
                                   remaining_ms)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return false;
}

bool bzm_serial_read_register(bzm_serial_transport_t *transport,
                              uint8_t asic_id, uint16_t engine_id,
                              uint8_t offset, void *data,
                              size_t data_len)
{
    if (transport == NULL) return false;
    return serial_read_register_op(transport, asic_id, engine_id, offset,
                                   data, data_len);
}

static void serial_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

size_t bzm_serial_discover_chain(bzm_serial_transport_t *transport,
                                 size_t expected_count)
{
    if (transport == NULL) return 0;
    static const bzm_chain_ops_t ops = {
        .noop = serial_noop,
        .write_register = serial_write_register_op,
        .read_register = serial_read_register_op,
        .delay_ms = serial_delay_ms,
    };
    SERIAL_clear_buffer();
    PENDING_RESULT_HEAD = 0;
    PENDING_RESULT_LENGTH = 0;
    transport->asic_count = bzm_discover_chain(
        expected_count, BZM_FIRST_ASIC_ID, transport->asic_ids,
        sizeof(transport->asic_ids), &ops, transport);
    return transport->asic_count;
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
    serial_register_context_t *writer = context;
    return writer != NULL && serial_write_register_raw(
        writer->asic_id, engine_id, offset, data, data_len);
}

bool bzm_serial_write_work(void *context, const bzm_work_t *work)
{
    bzm_serial_transport_t *transport = context;
    if (transport == NULL || work == NULL || transport->asic_count == 0) {
        return false;
    }

    for (size_t i = 0; i < transport->asic_count; ++i) {
        bzm_work_t chip_work = *work;
        if (!bzm_partition_nonce_range(
                work->starting_nonce, work->end_nonce, i,
                transport->asic_count, &chip_work.starting_nonce,
                &chip_work.end_nonce)) {
            return false;
        }
        serial_register_context_t writer = {
            .asic_id = transport->asic_ids[i],
        };
        if (!bzm_transport_program_work(&chip_work,
                                        serial_register_writer, &writer)) {
            return false;
        }
    }
    return true;
}

bool bzm_serial_flush(void *context)
{
    bzm_serial_transport_t *transport = context;
    if (transport == NULL) return false;

    bool success = transport->asic_count != 0;
    for (size_t i = 0; success && i < transport->asic_count; ++i) {
        serial_register_context_t writer = {
            .asic_id = transport->asic_ids[i],
        };
        success = bzm_transport_program_flush(
            transport->engine_count, transport->enhanced_mode,
            serial_register_writer, &writer);
    }
    SERIAL_clear_buffer();
    return success;
}

bool bzm_serial_read_result(bzm_raw_result_t *result, uint16_t timeout_ms)
{
    if (dequeue_result(result)) return true;

    uint8_t header[2];
    if (!read_tdm_header(header, timeout_ms) || header[1] != 0x01) {
        return false;
    }
    uint8_t frame[BZM_TDM_RESULT_FRAME_SIZE] = {
        header[0], header[1],
    };
    return serial_read_exact(frame + 2, BZM_RESULT_FRAME_SIZE, timeout_ms) &&
           bzm_tdm_result_decode(frame, esp_timer_get_time(), result);
}

const bzm_transport_ops_t BZM_SERIAL_TRANSPORT_OPS = {
    .write_work = bzm_serial_write_work,
    .flush = bzm_serial_flush,
};
