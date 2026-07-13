#include "bm_job_builder.h"

#include <stddef.h>
#include <string.h>

#include "utils.h"

bool bm_job_build(const mining_template_t *template, bm_job *job)
{
    if (template == NULL || job == NULL) return false;

    memset(job, 0, sizeof(*job));
    job->version = template->version;
    job->version_mask = template->version_mask;
    job->ntime = template->ntime;
    job->target = template->target;
    job->starting_nonce = template->starting_nonce;
    memcpy(job->prev_block_hash, template->prev_block_hash,
           sizeof(job->prev_block_hash));
    memcpy(job->merkle_root, template->merkle_root,
           sizeof(job->merkle_root));

    uint8_t header_prev_hash[32];
    uint8_t header_merkle_root[32];
    reverse_32bit_words(template->prev_block_hash, header_prev_hash);
    reverse_32bit_words(template->merkle_root, header_merkle_root);

    uint8_t midstate_data[64];
    uint8_t midstate[32];
    uint32_t rolled_version = template->version;
    memcpy(midstate_data, &rolled_version, 4);
    memcpy(midstate_data + 4, header_prev_hash, 32);
    memcpy(midstate_data + 36, header_merkle_root, 28);
    midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
    reverse_32bit_words(midstate, job->midstate);

    if (template->version_mask == 0) {
        job->num_midstates = 1;
        return true;
    }

    uint8_t *destinations[] = {
        job->midstate1, job->midstate2, job->midstate3,
    };
    for (size_t i = 0; i < 3; ++i) {
        rolled_version = increment_bitmask(rolled_version,
                                           template->version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
        reverse_32bit_words(midstate, destinations[i]);
    }
    job->num_midstates = 4;
    return true;
}
