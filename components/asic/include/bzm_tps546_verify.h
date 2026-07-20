#ifndef BZM_TPS546_VERIFY_H
#define BZM_TPS546_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BZM_TPS546_VERIFY_GOOD_DETAIL "GOOD"

typedef struct {
    const char *name;
    const uint8_t *expected;
    size_t expected_len;
    const uint8_t *observed;
    size_t observed_len;
} bzm_tps546_verify_field_t;

/**
 * Compare exact encoded TPS546 register values in declaration order.
 *
 * The first mismatch is returned in detail. One- and two-byte fields are
 * rendered as complete encoded values; longer blocks identify the first
 * differing byte. Length differences are treated as malformed readback.
 */
esp_err_t bzm_tps546_verify_fields(
    const bzm_tps546_verify_field_t *fields, size_t field_count,
    char *detail, size_t detail_len);

#endif /* BZM_TPS546_VERIFY_H */
