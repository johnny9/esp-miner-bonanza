#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"

#include "esp_log.h"
#include "soc/uart_struct.h"

#include "serial.h"
#include "utils.h"

#define ECHO_TEST_TXD (17)
#define ECHO_TEST_RXD (18)

static const char * TAG = "serial";

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
    return uart_flush_input(UART_NUM_1);
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
int16_t SERIAL_rx(uint8_t * buf, uint16_t size, uint16_t timeout_ms)
{
    int16_t bytes_read = uart_read_bytes(UART_NUM_1, buf, size, timeout_ms / portTICK_PERIOD_MS);

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
}
