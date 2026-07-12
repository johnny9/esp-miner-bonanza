#ifndef BM_JOB_BUILDER_H_
#define BM_JOB_BUILDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "mining.h"
#include "sv2_protocol.h"

// Internal, Bitmain-family work conversion helpers. bm_job is the temporary
// representation consumed by the BM13xx drivers; it is not a generic ASIC API.
bool bm_job_build_sv1(const mining_notify *notification,
                      const char *extranonce_prefix,
                      uint32_t extranonce2_len,
                      uint64_t extranonce2_counter,
                      uint32_t version_mask,
                      double difficulty,
                      bm_job *job);

bool bm_job_build_sv2_standard(const sv2_job_t *source,
                               uint32_t version_mask,
                               double difficulty,
                               bm_job *job);

bool bm_job_build_sv2_extended(const sv2_ext_job_t *source,
                               const sv2_conn_t *connection,
                               uint64_t extranonce2_counter,
                               uint32_t version_mask,
                               double difficulty,
                               bm_job *job);

// Standard-channel work is only generated for a newly dequeued job. A timeout
// must not restart the ASIC nonce search on the same work.
bool bm_job_should_generate_sv2_standard(bool dequeued_new_work);

#endif // BM_JOB_BUILDER_H_
