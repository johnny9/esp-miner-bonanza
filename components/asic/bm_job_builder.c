#include "bm_job_builder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

#define BM_MAX_EXTRANONCE2_LEN 32

static bool duplicate_metadata(bm_job *job, const char *jobid, const char *extranonce2)
{
    job->jobid = strdup(jobid);
    job->extranonce2 = strdup(extranonce2);
    return job->jobid != NULL && job->extranonce2 != NULL;
}

static void build_midstates(bm_job *job, const uint8_t prev_hash[32],
                            const uint8_t merkle_root[32], uint32_t version_mask)
{
    uint8_t midstate_data[64];
    uint8_t midstate[32];
    uint32_t rolled_version = job->version;

    memcpy(midstate_data, &rolled_version, 4);
    memcpy(midstate_data + 4, prev_hash, 32);
    memcpy(midstate_data + 36, merkle_root, 28);
    midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
    reverse_32bit_words(midstate, job->midstate);

    if (version_mask == 0) {
        job->num_midstates = 1;
        return;
    }

    uint8_t *destinations[] = {job->midstate1, job->midstate2, job->midstate3};
    for (size_t i = 0; i < 3; ++i) {
        rolled_version = increment_bitmask(rolled_version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
        reverse_32bit_words(midstate, destinations[i]);
    }
    job->num_midstates = 4;
}

bool bm_job_build_sv1(const mining_notify *notification,
                      const char *extranonce_prefix,
                      uint32_t extranonce2_len,
                      uint64_t extranonce2_counter,
                      uint32_t version_mask,
                      double difficulty,
                      bm_job *job)
{
    if (job == NULL) return false;
    memset(job, 0, sizeof(*job));
    if (notification == NULL || extranonce_prefix == NULL ||
        extranonce2_len > BM_MAX_EXTRANONCE2_LEN) {
        return false;
    }

    char extranonce2[BM_MAX_EXTRANONCE2_LEN * 2 + 1];
    extranonce_2_generate(extranonce2_counter, extranonce2_len, extranonce2);

    uint8_t coinbase_hash[32];
    uint8_t merkle_root[32];
    calculate_coinbase_tx_hash(notification->coinbase_1, notification->coinbase_2,
                               extranonce_prefix, extranonce2, coinbase_hash);
    calculate_merkle_root_hash(coinbase_hash,
                               (const uint8_t (*)[32])notification->merkle_branches,
                               notification->n_merkle_branches, merkle_root);
    construct_bm_job((mining_notify *)notification, merkle_root, version_mask,
                     difficulty, job);
    job->version_mask = version_mask;
    return duplicate_metadata(job, notification->job_id, extranonce2);
}

bool bm_job_build_sv2_standard(const sv2_job_t *source,
                               uint32_t version_mask,
                               double difficulty,
                               bm_job *job)
{
    if (job == NULL) return false;
    memset(job, 0, sizeof(*job));
    if (source == NULL) return false;

    job->version = source->version;
    job->version_mask = version_mask;
    job->target = source->nbits;
    job->ntime = source->ntime;
    job->pool_diff = difficulty;
    reverse_32bit_words(source->merkle_root, job->merkle_root);
    reverse_32bit_words(source->prev_hash, job->prev_block_hash);
    build_midstates(job, source->prev_hash, source->merkle_root, version_mask);

    char jobid[16];
    snprintf(jobid, sizeof(jobid), "%" PRIu32, source->job_id);
    return duplicate_metadata(job, jobid, "");
}

bool bm_job_build_sv2_extended(const sv2_ext_job_t *source,
                               const sv2_conn_t *connection,
                               uint64_t extranonce2_counter,
                               uint32_t version_mask,
                               double difficulty,
                               bm_job *job)
{
    if (job == NULL) return false;
    memset(job, 0, sizeof(*job));
    if (source == NULL || connection == NULL ||
        connection->extranonce_prefix_len > sizeof(connection->extranonce_prefix) ||
        connection->extranonce_size > BM_MAX_EXTRANONCE2_LEN ||
        source->merkle_path_count > SV2_MAX_MERKLE_BRANCHES) {
        return false;
    }

    uint8_t extranonce2[BM_MAX_EXTRANONCE2_LEN] = {0};
    for (int i = connection->extranonce_size - 1;
         i >= 0 && extranonce2_counter > 0; --i) {
        extranonce2[i] = (uint8_t)(extranonce2_counter & 0xff);
        extranonce2_counter >>= 8;
    }

    uint8_t coinbase_hash[32];
    uint8_t merkle_root[32];
    calculate_coinbase_tx_hash_bin(source->coinbase_prefix, source->coinbase_prefix_len,
                                   connection->extranonce_prefix,
                                   connection->extranonce_prefix_len,
                                   extranonce2, connection->extranonce_size,
                                   source->coinbase_suffix, source->coinbase_suffix_len,
                                   coinbase_hash);
    calculate_merkle_root_hash(coinbase_hash, source->merkle_path,
                               source->merkle_path_count, merkle_root);

    job->version = source->version;
    job->version_mask = version_mask;
    job->target = source->nbits;
    job->ntime = source->ntime;
    job->pool_diff = difficulty;
    reverse_32bit_words(merkle_root, job->merkle_root);
    reverse_32bit_words(source->prev_hash, job->prev_block_hash);
    build_midstates(job, source->prev_hash, merkle_root, version_mask);

    char jobid[16];
    char extranonce2_hex[BM_MAX_EXTRANONCE2_LEN * 2 + 1];
    snprintf(jobid, sizeof(jobid), "%" PRIu32, source->job_id);
    bin2hex(extranonce2, connection->extranonce_size, extranonce2_hex,
            sizeof(extranonce2_hex));
    return duplicate_metadata(job, jobid, extranonce2_hex);
}

bool bm_job_should_generate_sv2_standard(bool dequeued_new_work)
{
    return dequeued_new_work;
}
