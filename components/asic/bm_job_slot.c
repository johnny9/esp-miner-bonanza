#include "bm_job_slot.h"

#include <stdlib.h>
#include <string.h>

bm_job *bm_job_slot_snapshot_locked(const uint8_t *valid_jobs,
                                    bm_job *const *active_jobs,
                                    uint8_t job_id)
{
    if (valid_jobs == NULL || active_jobs == NULL || job_id >= BM_JOB_SLOT_COUNT) {
        return NULL;
    }

    bm_job *source = active_jobs[job_id];
    if (valid_jobs[job_id] == 0 || source == NULL) {
        return NULL;
    }

    bm_job *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot != NULL) {
        *snapshot = *source;
        snapshot->jobid = source->jobid ? strdup(source->jobid) : NULL;
        snapshot->extranonce2 = source->extranonce2 ? strdup(source->extranonce2) : NULL;
        if ((source->jobid != NULL && snapshot->jobid == NULL) ||
            (source->extranonce2 != NULL && snapshot->extranonce2 == NULL)) {
            free_bm_job(snapshot);
            snapshot = NULL;
        }
    }
    return snapshot;
}

bm_job *bm_job_slot_snapshot(pthread_mutex_t *lock, const uint8_t *valid_jobs,
                             bm_job *const *active_jobs, uint8_t job_id)
{
    if (lock == NULL) return NULL;
    pthread_mutex_lock(lock);
    bm_job *snapshot =
        bm_job_slot_snapshot_locked(valid_jobs, active_jobs, job_id);
    pthread_mutex_unlock(lock);
    return snapshot;
}
