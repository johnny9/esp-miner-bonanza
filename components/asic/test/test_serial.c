#include "driver/uart.h"
#include "serial.h"
#include "unity.h"

TEST_CASE("staged serial setup installs UART1 before selecting ASIC baud", "[asic][bzm][serial][not-on-qemu]")
{
    if (SERIAL_is_initialized()) {
        TEST_ASSERT_EQUAL(ESP_OK, uart_driver_delete(UART_NUM_1));
    }

    TEST_ASSERT_FALSE(SERIAL_is_initialized());
    TEST_ASSERT_EQUAL(ESP_OK, SERIAL_prepare_session(5000000));
    TEST_ASSERT_TRUE(SERIAL_is_initialized());

    // Repeating the call verifies that a new staged session can flush stale
    // RX data idempotently once the driver is installed. This test needs a
    // real ESP32-S3 UART peripheral; Espressif QEMU currently asserts while
    // configuring these console pins.
    TEST_ASSERT_EQUAL(ESP_OK, SERIAL_prepare_session(5000000));
    TEST_ASSERT_TRUE(SERIAL_is_initialized());
}
