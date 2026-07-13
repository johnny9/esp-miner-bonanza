#ifndef BZM_TRANSPORT_H
#define BZM_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bzm.h"
#include "bzm_reactor.h"

#define BZM_BROADCAST_ASIC 0xfa

typedef struct {
    uint8_t asic_ids[BZM_MAX_ASIC_COUNT];
    uint8_t asic_count;
    uint16_t engine_count;
    bool enhanced_mode;
} bzm_serial_transport_t;

typedef struct {
    bool (*noop)(void *context, uint8_t asic_id);
    bool (*write_register)(void *context, uint8_t asic_id,
                           uint16_t engine_id, uint8_t offset,
                           const void *data, size_t data_len);
    bool (*read_register)(void *context, uint8_t asic_id,
                          uint16_t engine_id, uint8_t offset,
                          void *data, size_t data_len);
    void (*delay_ms)(void *context, uint32_t delay_ms);
} bzm_chain_ops_t;

typedef bool (*bzm_register_writer_t)(void *context, uint16_t engine_id,
                                      uint8_t offset, const void *data,
                                      size_t data_len);

size_t bzm_transport_encode_write(uint8_t asic_id, uint16_t engine_id,
                                  uint8_t offset, const uint8_t *data,
                                  size_t data_len, uint8_t *encoded,
                                  size_t encoded_capacity);
size_t bzm_transport_encode_read(uint8_t asic_id, uint16_t engine_id,
                                 uint8_t offset, size_t data_len,
                                 uint8_t *encoded,
                                 size_t encoded_capacity);
size_t bzm_transport_encode_noop(uint8_t asic_id, uint8_t *encoded,
                                 size_t encoded_capacity);

size_t bzm_discover_chain(size_t expected_count, uint8_t first_asic_id,
                          uint8_t *asic_ids, size_t asic_ids_capacity,
                          const bzm_chain_ops_t *ops, void *ops_context);
bool bzm_partition_nonce_range(uint32_t starting_nonce, uint32_t end_nonce,
                               size_t partition, size_t partition_count,
                               uint32_t *partition_start,
                               uint32_t *partition_end);
size_t bzm_serial_discover_chain(bzm_serial_transport_t *transport,
                                 size_t expected_count);

bool bzm_serial_write_register(bzm_serial_transport_t *transport,
                               uint16_t engine_id, uint8_t offset,
                               const void *data, size_t data_len);
bool bzm_serial_write_register_to(bzm_serial_transport_t *transport,
                                  uint8_t asic_id, uint16_t engine_id,
                                  uint8_t offset, const void *data,
                                  size_t data_len);
bool bzm_serial_read_register(bzm_serial_transport_t *transport,
                              uint8_t asic_id, uint16_t engine_id,
                              uint8_t offset, void *data,
                              size_t data_len);
bool bzm_transport_program_work(const bzm_work_t *work,
                                bzm_register_writer_t writer,
                                void *writer_context);
bool bzm_transport_program_flush(uint16_t engine_count, bool enhanced_mode,
                                 bzm_register_writer_t writer,
                                 void *writer_context);
bool bzm_serial_write_work(void *context, const bzm_work_t *work);
bool bzm_serial_flush(void *context);
bool bzm_serial_read_result(bzm_raw_result_t *result, uint16_t timeout_ms);

extern const bzm_transport_ops_t BZM_SERIAL_TRANSPORT_OPS;

#endif // BZM_TRANSPORT_H
