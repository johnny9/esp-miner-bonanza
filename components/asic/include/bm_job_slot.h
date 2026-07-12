#ifndef BM_JOB_SLOT_H_
#define BM_JOB_SLOT_H_

#include <pthread.h>
#include <stdint.h>

#include "mining.h"

#define BM_JOB_SLOT_COUNT 128

// Return a caller-owned deep snapshot, or NULL for an invalid/null slot.
bm_job *bm_job_slot_snapshot(pthread_mutex_t *lock, const uint8_t *valid_jobs,
                             bm_job *const *active_jobs, uint8_t job_id);

// Copy a slot while the caller already holds the slot mutex. Exposed only so
// the locking invariant can be exercised deterministically by component tests.
bm_job *bm_job_slot_snapshot_locked(const uint8_t *valid_jobs,
                                    bm_job *const *active_jobs,
                                    uint8_t job_id);

#endif // BM_JOB_SLOT_H_
