#ifndef ASIC_JOB_STORE_H
#define ASIC_JOB_STORE_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>

#include "asic_result.h"
#include "mining.h"

/* BZM keeps one independently generated job active for each of its 236
 * logical engines. Keep a full 8-bit hardware-handle slot space so every
 * current assignment remains addressable while the scheduler rotates. */
#define ASIC_JOB_STORE_CAPACITY 256

typedef struct {
    bool valid;
    asic_work_handle_t handle;
    mining_template_t template;
} asic_job_store_entry_t;

typedef struct {
    pthread_mutex_t lock;
    asic_job_store_entry_t entries[ASIC_JOB_STORE_CAPACITY];
    uint16_t next_slot;
    uint64_t next_generation;
} asic_job_store_t;

bool asic_job_store_init(asic_job_store_t *store);
void asic_job_store_destroy(asic_job_store_t *store);

// Compatibility mode for ASICs whose hardware result contains only a slot id.
bool asic_job_store_store_slot(asic_job_store_t *store, uint8_t slot,
                               const mining_template_t *template,
                               asic_work_handle_t *handle);

// Generation-bearing handles reject stale results after reuse or invalidation.
bool asic_job_store_store_generated(asic_job_store_t *store,
                                    const mining_template_t *template,
                                    asic_work_handle_t *handle);

bool asic_job_store_snapshot(asic_job_store_t *store,
                             asic_work_handle_t handle,
                             mining_template_t *snapshot);
bool asic_job_store_release(asic_job_store_t *store,
                            asic_work_handle_t handle);
void asic_job_store_invalidate_all(asic_job_store_t *store);

#endif // ASIC_JOB_STORE_H
