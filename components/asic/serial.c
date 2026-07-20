#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "soc/uart_struct.h"

#include "bzm_data_link.h"
#include "serial.h"
#include "utils.h"

#define ECHO_TEST_TXD (17)
#define ECHO_TEST_RXD (18)

static const char * TAG = "serial";
static bzm_data_link_decoder_t DATA_LINK_DECODER;
static uint8_t DATA_LINK_WIRE_BUFFER[256];
static size_t DATA_LINK_WIRE_OFFSET;
static size_t DATA_LINK_WIRE_LENGTH;

static void reset_data_link_stream(void)
{
    DATA_LINK_WIRE_OFFSET = 0;
    DATA_LINK_WIRE_LENGTH = 0;
    bzm_data_link_decoder_reset_stream(&DATA_LINK_DECODER);
}

esp_err_t SERIAL_init(void)
{
    ESP_LOGI(TAG, "Initializing serial");
    // Configure UART1 parameters
    uart_config_t uart_config = {
        .baud_rate = UART_FREQ,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };
    // Configure UART1 parameters
    esp_err_t err = uart_param_config(UART_NUM_1, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    // Set UART1 pins(TX: IO17, RX: I018)
    err = uart_set_pin(UART_NUM_1, ECHO_TEST_TXD, ECHO_TEST_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    // Install the UART driver without an event queue.  The larger RX ring
    // preserves TDM traffic while a full work dispatch owns the reactor lock.
    err = uart_driver_install(
        UART_NUM_1, SERIAL_RX_BUFFER_BYTES, SERIAL_TX_BUFFER_BYTES, 0, NULL, 0
    );
    if (err != ESP_OK) {
        return err;
    }

    /* The ESP32-S3 UART FIFO is only 128 bytes. At 5 Mbaud the driver's
     * near-full default threshold leaves only microseconds for its ISR,
     * which is not enough while the command path briefly masks interrupts.
     * Trigger at 32 bytes so the remaining 96 bytes absorb bounded latency
     * before the 32 KiB software ring takes ownership. */
    err = uart_set_rx_full_threshold(UART_NUM_1,
                                     SERIAL_RX_FIFO_FULL_THRESHOLD);
    if (err != ESP_OK) {
        (void) uart_driver_delete(UART_NUM_1);
        return err;
    }
    bzm_data_link_decoder_init(&DATA_LINK_DECODER);
    DATA_LINK_WIRE_OFFSET = 0;
    DATA_LINK_WIRE_LENGTH = 0;
    return ESP_OK;
}

bool SERIAL_is_initialized(void)
{
    return uart_is_driver_installed(UART_NUM_1);
}

esp_err_t SERIAL_set_baud(int baud)
{
    ESP_LOGI(TAG, "Changing UART baud to %i", baud);

    // Make sure that we are done writing before setting a new baudrate.
    esp_err_t err = uart_wait_tx_done(UART_NUM_1, 1000 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        return err;
    }

    return uart_set_baudrate(UART_NUM_1, baud);
}

esp_err_t SERIAL_wait_tx_done(uint32_t timeout_ms)
{
    if (!SERIAL_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    return uart_wait_tx_done(UART_NUM_1, timeout_ticks);
}

esp_err_t SERIAL_ensure_initialized(int baud)
{
    if (!SERIAL_is_initialized()) {
        esp_err_t err = SERIAL_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    return SERIAL_set_baud(baud);
}

esp_err_t SERIAL_prepare_session(int baud)
{
    esp_err_t err = SERIAL_ensure_initialized(baud);
    if (err != ESP_OK) {
        return err;
    }

    /* A stopped validation can leave complete result or TDM frames in the
     * ESP UART driver's RX ring.  The staged transport installs a fresh
     * parser for every run, so carrying those bytes into the next session
     * would make its first chain probe fail as malformed I/O. */
    err = uart_flush_input(UART_NUM_1);
    if (err == ESP_OK) reset_data_link_stream();
    return err;
}

int SERIAL_send(uint8_t * data, int len, bool debug)
{
    if (debug) {
        printf("tx: ");
        prettyHex((unsigned char *) data, len);
        printf("\n");
    }

    return uart_write_bytes(UART_NUM_1, (const char *) data, len);
}

/// @brief waits for a serial response from the device
/// @param buf buffer to read data into
/// @param buf number of ms to wait before timing out
/// @return number of bytes read, or -1 on error
static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0) return 0;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

static int read_wire_bytes(uint32_t timeout_ms)
{
    DATA_LINK_WIRE_OFFSET = 0;
    DATA_LINK_WIRE_LENGTH = 0;

    size_t buffered = 0;
    if (uart_get_buffered_data_len(UART_NUM_1, &buffered) != ESP_OK) return -1;
    if (buffered == 0 && timeout_ms != 0) {
        int first = uart_read_bytes(UART_NUM_1, DATA_LINK_WIRE_BUFFER, 1,
                                    timeout_ticks(timeout_ms));
        if (first <= 0) return first;
        DATA_LINK_WIRE_LENGTH = (size_t)first;
        if (uart_get_buffered_data_len(UART_NUM_1, &buffered) != ESP_OK) return -1;
    }

    size_t capacity = sizeof(DATA_LINK_WIRE_BUFFER) - DATA_LINK_WIRE_LENGTH;
    if (buffered > capacity) buffered = capacity;
    if (buffered != 0) {
        int drained = uart_read_bytes(UART_NUM_1,
            DATA_LINK_WIRE_BUFFER + DATA_LINK_WIRE_LENGTH, buffered, 0);
        if (drained < 0) return drained;
        DATA_LINK_WIRE_LENGTH += (size_t)drained;
    }
    return (int)DATA_LINK_WIRE_LENGTH;
}

int16_t SERIAL_rx_marked(uint8_t *buf, uint8_t *ninth_bits, uint16_t size,
                         uint16_t timeout_ms)
{
    if (buf == NULL || size == 0 || !SERIAL_is_initialized()) return -1;

    size_t decoded = bzm_data_link_decoder_read(&DATA_LINK_DECODER, buf,
                                                 ninth_bits, size);
    if (decoded != 0) return (int16_t)decoded;

    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (true) {
        if (DATA_LINK_WIRE_OFFSET < DATA_LINK_WIRE_LENGTH) {
            size_t consumed = bzm_data_link_decoder_feed(
                &DATA_LINK_DECODER,
                DATA_LINK_WIRE_BUFFER + DATA_LINK_WIRE_OFFSET,
                DATA_LINK_WIRE_LENGTH - DATA_LINK_WIRE_OFFSET);
            DATA_LINK_WIRE_OFFSET += consumed;
            decoded = bzm_data_link_decoder_read(&DATA_LINK_DECODER, buf,
                                                  ninth_bits, size);
            if (decoded != 0) return (int16_t)decoded;
            if (DATA_LINK_WIRE_OFFSET < DATA_LINK_WIRE_LENGTH && consumed == 0)
                return -1;
            continue;
        }

        uint32_t remaining_ms = 0;
        if (timeout_ms != 0) {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) return 0;
            remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        }
        int wire_read = read_wire_bytes(remaining_ms);
        if (wire_read <= 0) return (int16_t)wire_read;
    }
}

int16_t SERIAL_rx(uint8_t * buf, uint16_t size, uint16_t timeout_ms)
{
    int16_t bytes_read = SERIAL_rx_marked(buf, NULL, size, timeout_ms);

#if BM1397_SERIALRX_DEBUG || BM1366_SERIALRX_DEBUG || BM1368_SERIALRX_DEBUG || BM1370_SERIALRX_DEBUG
    size_t buff_len = 0;
    if (bytes_read > 0) {
        uart_get_buffered_data_len(UART_NUM_1, &buff_len);
        printf("rx: ");
        prettyHex((unsigned char *) buf, bytes_read);
        printf(" [%d]\n", buff_len);
    }
#endif

    return bytes_read;
}

bool SERIAL_get_data_link_stats(bzm_data_link_stats_t *stats)
{
    if (stats == NULL) return false;
    const bzm_data_link_stats_t *current =
        bzm_data_link_decoder_stats(&DATA_LINK_DECODER);
    if (current == NULL) return false;
    *stats = *current;
    return true;
}

void SERIAL_debug_rx(void)
{
    int ret;
    uint8_t buf[100];

    ret = SERIAL_rx(buf, 100, 20);
    if (ret < 0) {
        fprintf(stderr, "unable to read data\n");
        return;
    }

    memset(buf, 0, 100);
}

void SERIAL_clear_buffer(void)
{
    uart_flush(UART_NUM_1);
    reset_data_link_stream();
}
