#include "sv2_mining_template.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

static bool duplicate_metadata(mining_template_t *template,
                               const char *job_id, const char *extranonce2)
{
    template->share.job_id = strdup(job_id ? job_id : "");
    template->share.extranonce2 = strdup(extranonce2 ? extranonce2 : "");
    if (template->share.job_id == NULL ||
        template->share.extranonce2 == NULL) {
        mining_template_free(template);
        return false;
    }
    return true;
}

bool mining_template_build_sv2_standard(const sv2_job_t *source,
                                        uint32_t version_mask,
                                        double difficulty,
                                        mining_template_t *template)
{
    if (template == NULL) return false;
    memset(template, 0, sizeof(*template));
    if (source == NULL) return false;

    template->version = source->version;
    template->version_mask = version_mask;
    template->target = source->nbits;
    template->ntime = source->ntime;
    template->clean_jobs = source->clean_jobs;
    template->share.protocol = MINING_PROTOCOL_SV2_STANDARD;
    template->share.numeric_job_id = source->job_id;
    template->share.pool_difficulty = difficulty;
    reverse_32bit_words(source->merkle_root, template->merkle_root);
    reverse_32bit_words(source->prev_hash, template->prev_block_hash);

    char job_id[16];
    snprintf(job_id, sizeof(job_id), "%" PRIu32, source->job_id);
    return duplicate_metadata(template, job_id, "");
}

bool mining_template_build_sv2_extended(const sv2_ext_job_t *source,
                                        const sv2_conn_t *connection,
                                        uint64_t extranonce2_counter,
                                        uint32_t version_mask,
                                        double difficulty,
                                        mining_template_t *template)
{
    if (template == NULL) return false;
    memset(template, 0, sizeof(*template));
    if (source == NULL || connection == NULL ||
        connection->extranonce_prefix_len >
            sizeof(connection->extranonce_prefix) ||
        connection->extranonce_size > MINING_MAX_EXTRANONCE2_SIZE ||
        source->merkle_path_count > SV2_MAX_MERKLE_BRANCHES) {
        return false;
    }

    uint8_t extranonce2[MINING_MAX_EXTRANONCE2_SIZE] = {0};
    for (int i = connection->extranonce_size - 1;
         i >= 0 && extranonce2_counter > 0; --i) {
        extranonce2[i] = (uint8_t)(extranonce2_counter & 0xff);
        extranonce2_counter >>= 8;
    }

    uint8_t coinbase_hash[32];
    uint8_t merkle_root[32];
    calculate_coinbase_tx_hash_bin(
        source->coinbase_prefix, source->coinbase_prefix_len,
        connection->extranonce_prefix, connection->extranonce_prefix_len,
        extranonce2, connection->extranonce_size,
        source->coinbase_suffix, source->coinbase_suffix_len,
        coinbase_hash);
    calculate_merkle_root_hash(coinbase_hash, source->merkle_path,
                               source->merkle_path_count, merkle_root);

    template->version = source->version;
    template->version_mask = version_mask;
    template->target = source->nbits;
    template->ntime = source->ntime;
    template->clean_jobs = source->clean_jobs;
    template->share.protocol = MINING_PROTOCOL_SV2_EXTENDED;
    template->share.numeric_job_id = source->job_id;
    template->share.extranonce2_len = connection->extranonce_size;
    template->share.pool_difficulty = difficulty;
    memcpy(template->share.extranonce2_bin, extranonce2,
           connection->extranonce_size);
    reverse_32bit_words(merkle_root, template->merkle_root);
    reverse_32bit_words(source->prev_hash, template->prev_block_hash);

    char job_id[16];
    char extranonce2_hex[MINING_MAX_EXTRANONCE2_SIZE * 2 + 1];
    snprintf(job_id, sizeof(job_id), "%" PRIu32, source->job_id);
    bin2hex(extranonce2, connection->extranonce_size, extranonce2_hex,
            sizeof(extranonce2_hex));
    return duplicate_metadata(template, job_id, extranonce2_hex);
}
