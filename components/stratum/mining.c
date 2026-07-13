#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include "mining.h"
#include "utils.h"
#include "mbedtls/sha256.h"
#include "esp_log.h"

void mining_template_free(mining_template_t *template)
{
    if (template == NULL) return;
    free(template->share.job_id);
    free(template->share.extranonce2);
    memset(template, 0, sizeof(*template));
}

bool mining_template_clone(const mining_template_t *source,
                           mining_template_t *destination)
{
    if (source == NULL || destination == NULL) return false;

    memset(destination, 0, sizeof(*destination));
    *destination = *source;
    destination->share.job_id = source->share.job_id
        ? strdup(source->share.job_id) : NULL;
    destination->share.extranonce2 = source->share.extranonce2
        ? strdup(source->share.extranonce2) : NULL;
    if ((source->share.job_id != NULL && destination->share.job_id == NULL) ||
        (source->share.extranonce2 != NULL &&
         destination->share.extranonce2 == NULL)) {
        mining_template_free(destination);
        return false;
    }
    return true;
}

void calculate_coinbase_tx_hash(const char *coinbase_1, const char *coinbase_2, const char *extranonce, const char *extranonce_2, uint8_t dest[32])
{
    size_t len1 = strlen(coinbase_1);
    size_t len2 = strlen(extranonce);
    size_t len3 = strlen(extranonce_2);
    size_t len4 = strlen(coinbase_2);

    size_t coinbase_tx_bin_len = (len1 + len2 + len3 + len4) / 2;

    uint8_t coinbase_tx_bin[coinbase_tx_bin_len];

    size_t bin_offset = 0;
    bin_offset += hex2bin(coinbase_1, coinbase_tx_bin + bin_offset, coinbase_tx_bin_len - bin_offset);
    bin_offset += hex2bin(extranonce, coinbase_tx_bin + bin_offset, coinbase_tx_bin_len - bin_offset);
    bin_offset += hex2bin(extranonce_2, coinbase_tx_bin + bin_offset, coinbase_tx_bin_len - bin_offset);
    bin_offset += hex2bin(coinbase_2, coinbase_tx_bin + bin_offset, coinbase_tx_bin_len - bin_offset);

    double_sha256_bin(coinbase_tx_bin, coinbase_tx_bin_len, dest);
}

void calculate_coinbase_tx_hash_bin(const uint8_t *prefix, size_t prefix_len,
                                    const uint8_t *extranonce_prefix, size_t ep_len,
                                    const uint8_t *extranonce_2, size_t e2_len,
                                    const uint8_t *suffix, size_t suffix_len,
                                    uint8_t dest[32])
{
    size_t total_len = prefix_len + ep_len + e2_len + suffix_len;
    uint8_t *buf = malloc(total_len);
    if (!buf) return;

    size_t offset = 0;
    memcpy(buf + offset, prefix, prefix_len);   offset += prefix_len;
    memcpy(buf + offset, extranonce_prefix, ep_len); offset += ep_len;
    memcpy(buf + offset, extranonce_2, e2_len); offset += e2_len;
    memcpy(buf + offset, suffix, suffix_len);

    double_sha256_bin(buf, total_len, dest);
    free(buf);
}

void calculate_merkle_root_hash(const uint8_t coinbase_tx_hash[32], const uint8_t merkle_branches[][32], const int num_merkle_branches, uint8_t dest[32])
{
    uint8_t both_merkles[64];
    memcpy(both_merkles, coinbase_tx_hash, 32);
    for (int i = 0; i < num_merkle_branches; i++) {
        memcpy(both_merkles + 32, merkle_branches[i], 32);
        double_sha256_bin(both_merkles, 64, both_merkles);
    }

    memcpy(dest, both_merkles, 32);
}

void extranonce_2_generate(uint64_t extranonce_2, uint32_t length, char dest[static length * 2 + 1])
{
    // Allocate buffer to hold the extranonce_2 value in bytes
    uint8_t extranonce_2_bytes[length];
    memset(extranonce_2_bytes, 0, length);
    
    // Copy the extranonce_2 value into the buffer, handling endianness
    // Copy up to the size of uint64_t or the requested length, whichever is smaller
    size_t copy_len = (length < sizeof(uint64_t)) ? length : sizeof(uint64_t);
    memcpy(extranonce_2_bytes, &extranonce_2, copy_len);
    
    // Convert the bytes to hex string
    bin2hex(extranonce_2_bytes, length, dest, length * 2 + 1);
}

double hash_to_pdiff(const uint8_t hash[32])
{
    double s64 = le256todouble(hash);
    if (s64 == 0.0) return (double)UINT32_MAX;
    return truediffone / s64;
}

///////cgminer nonce testing
/* testing a nonce and return the diff - 0 means invalid */
double mining_test_nonce_value(const mining_template_t *template,
                               uint32_t nonce, uint32_t final_ntime,
                               uint32_t final_version)
{
    if (template == NULL) return 0;
    uint8_t header[80];

    // copy data from job to header
    memcpy(header, &final_version, 4);
    reverse_32bit_words(template->prev_block_hash, header + 4);
    reverse_32bit_words(template->merkle_root, header + 36);
    memcpy(header + 68, &final_ntime, 4);
    memcpy(header + 72, &template->target, 4);
    memcpy(header + 76, &nonce, 4);

    uint8_t hash_result[32];
    double_sha256_bin(header, 80, hash_result);

    return hash_to_pdiff(hash_result);
}

uint32_t increment_bitmask(const uint32_t value, const uint32_t mask)
{
    // if mask is zero, just return the original value
    if (mask == 0)
        return value;

    uint32_t carry = (value & mask) + (mask & -mask);      // increment the least significant bit of the mask
    uint32_t overflow = carry & ~mask;                     // find overflowed bits that are not in the mask
    uint32_t new_value = (value & ~mask) | (carry & mask); // set bits according to the mask

    // Handle carry propagation
    if (overflow > 0)
    {
        uint32_t carry_mask = (overflow << 1);                // shift left to get the mask where carry should be propagated
        new_value = increment_bitmask(new_value, carry_mask); // recursively handle carry propagation
    }

    return new_value;
}
