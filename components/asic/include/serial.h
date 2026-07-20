#ifndef SERIAL_H_
#define SERIAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "bzm_data_link.h"

typedef enum
{
    JOB_PACKET = 0,
    CMD_PACKET = 1,
} packet_type_t;

#define UART_FREQ 115200

/* A complete BZM work dispatch holds the reactor lock while all 944 engine
 * writes are issued.  The UART driver must retain the continuously arriving
 * TDM telemetry and result stream until the polling task can run again. */
#define SERIAL_RX_BUFFER_BYTES (32U * 1024U)
#define SERIAL_TX_BUFFER_BYTES (2U * 1024U)
#define SERIAL_RX_DESIGN_RATE_BYTES_PER_SECOND 16000U
#define SERIAL_RX_MAX_DISPATCH_BLACKOUT_MS 1250U
#define SERIAL_HARDWARE_FIFO_BYTES 128U
#define SERIAL_RX_FIFO_FULL_THRESHOLD 32U
#define SERIAL_WIRE_BYTES_PER_SECOND 500000U
#define SERIAL_MAX_ISR_LATENCY_US 150U

static inline bool SERIAL_buffer_capacity_covers(uint32_t buffer_bytes, uint32_t bytes_per_second,
                                                 uint32_t blackout_ms)
{
    return (uint64_t) buffer_bytes * 1000U >= (uint64_t) bytes_per_second * blackout_ms;
}

static inline bool SERIAL_fifo_reserve_covers(uint32_t fifo_bytes,
                                              uint32_t full_threshold,
                                              uint32_t bytes_per_second,
                                              uint32_t latency_us)
{
    if (full_threshold >= fifo_bytes || bytes_per_second == 0) {
        return false;
    }
    return (uint64_t) (fifo_bytes - full_threshold) * 1000000U >=
           (uint64_t) bytes_per_second * latency_us;
}

int SERIAL_send(uint8_t *, int, bool);
esp_err_t SERIAL_init(void);
esp_err_t SERIAL_ensure_initialized(int baud);
esp_err_t SERIAL_prepare_session(int baud);
void SERIAL_debug_rx(void);
int16_t SERIAL_rx(uint8_t *, uint16_t, uint16_t);
int16_t SERIAL_rx_marked(uint8_t *, uint8_t *, uint16_t, uint16_t);
bool SERIAL_get_data_link_stats(bzm_data_link_stats_t *stats);
void SERIAL_clear_buffer(void);
esp_err_t SERIAL_set_baud(int baud);
esp_err_t SERIAL_wait_tx_done(uint32_t timeout_ms);
bool SERIAL_is_initialized(void);

#endif /* SERIAL_H_ */
