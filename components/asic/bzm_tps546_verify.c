#include "bzm_tps546_verify.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0) {
        snprintf(detail, detail_len, "%s", text);
    }
}

static bool valid_field(const bzm_tps546_verify_field_t *field)
{
    return field->name != NULL && field->name[0] != '\0' &&
           field->expected != NULL && field->observed != NULL &&
           field->expected_len > 0 && field->observed_len > 0;
}

esp_err_t bzm_tps546_verify_fields(
    const bzm_tps546_verify_field_t *fields, size_t field_count,
    char *detail, size_t detail_len)
{
    if (detail == NULL || detail_len == 0) return ESP_ERR_INVALID_ARG;
    detail[0] = '\0';

    if (fields == NULL || field_count == 0) {
        set_detail(detail, detail_len, "INVALID_ARGUMENT");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t field_index = 0; field_index < field_count; ++field_index) {
        const bzm_tps546_verify_field_t *field = &fields[field_index];
        if (!valid_field(field)) {
            set_detail(detail, detail_len, "INVALID_ARGUMENT");
            return ESP_ERR_INVALID_ARG;
        }
        if (field->expected_len != field->observed_len) {
            snprintf(detail, detail_len, "%s length expected=%u observed=%u",
                     field->name, (unsigned)field->expected_len,
                     (unsigned)field->observed_len);
            return ESP_ERR_INVALID_SIZE;
        }

        size_t byte_index = 0;
        while (byte_index < field->expected_len &&
               field->expected[byte_index] == field->observed[byte_index]) {
            ++byte_index;
        }
        if (byte_index == field->expected_len) continue;

        if (field->expected_len == 1) {
            snprintf(detail, detail_len,
                     "%s expected=0x%02X observed=0x%02X", field->name,
                     field->expected[0], field->observed[0]);
        } else if (field->expected_len == 2) {
            uint16_t expected = (uint16_t)field->expected[0] |
                                ((uint16_t)field->expected[1] << 8);
            uint16_t observed = (uint16_t)field->observed[0] |
                                ((uint16_t)field->observed[1] << 8);
            snprintf(detail, detail_len,
                     "%s expected=0x%04X observed=0x%04X", field->name,
                     expected, observed);
        } else {
            snprintf(detail, detail_len,
                     "%s[%u] expected=0x%02X observed=0x%02X", field->name,
                     (unsigned)byte_index, field->expected[byte_index],
                     field->observed[byte_index]);
        }
        return ESP_FAIL;
    }

    set_detail(detail, detail_len, BZM_TPS546_VERIFY_GOOD_DETAIL);
    return ESP_OK;
}
