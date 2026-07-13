#ifndef MINING_H_
#define MINING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MINING_PROTOCOL_SV1 = 0,
    MINING_PROTOCOL_SV2_STANDARD,
    MINING_PROTOCOL_SV2_EXTENDED,
} mining_protocol_t;

#define MINING_MAX_EXTRANONCE2_SIZE 32

typedef struct {
    mining_protocol_t protocol;
    char *job_id;
    char *extranonce2;
    uint32_t numeric_job_id;
    uint8_t extranonce2_bin[MINING_MAX_EXTRANONCE2_SIZE];
    uint8_t extranonce2_len;
    double pool_difficulty;
} mining_share_metadata_t;

typedef struct {
    uint32_t version;
    uint32_t version_mask;
    // Stored in the same word order used by Bitcoin's 80-byte header helpers.
    uint8_t prev_block_hash[32];
    uint8_t merkle_root[32];
    uint32_t ntime;
    uint32_t target;
    uint32_t starting_nonce;
    bool clean_jobs;
    mining_share_metadata_t share;
} mining_template_t;

void mining_template_free(mining_template_t *template);
bool mining_template_clone(const mining_template_t *source,
                           mining_template_t *destination);

void calculate_coinbase_tx_hash(const char *coinbase_1, const char *coinbase_2,
                                const char *extranonce, const char *extranonce_2, uint8_t dest[32]);

void calculate_coinbase_tx_hash_bin(const uint8_t *prefix, size_t prefix_len,
                                    const uint8_t *extranonce_prefix, size_t ep_len,
                                    const uint8_t *extranonce_2, size_t e2_len,
                                    const uint8_t *suffix, size_t suffix_len,
                                    uint8_t dest[32]);

void calculate_merkle_root_hash(const uint8_t coinbase_tx_hash[32], const uint8_t merkle_branches[][32], const int num_merkle_branches, uint8_t dest[32]);

// Convert a 256-bit value (block hash or pool target, little-endian) to
// difficulty (pdiff = truediffone / value). Shared by SV1 (test_nonce_value)
// and SV2 (target). Returns a double to preserve fractional difficulty.
double hash_to_pdiff(const uint8_t hash[32]);

double mining_test_nonce_value(const mining_template_t *template,
                               uint32_t nonce, uint32_t final_ntime,
                               uint32_t final_version);

void extranonce_2_generate(uint64_t extranonce_2, uint32_t length, char dest[static length * 2 + 1]);

uint32_t increment_bitmask(const uint32_t value, const uint32_t mask);

#endif /* MINING_H_ */
