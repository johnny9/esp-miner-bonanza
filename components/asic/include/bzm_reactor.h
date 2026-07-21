#ifndef BZM_REACTOR_H
#define BZM_REACTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "asic_job_store.h"
#include "bzm.h"

typedef enum {
    BZM_ENGINE_IDLE = 0,
    BZM_ENGINE_ASSIGNED,
    BZM_ENGINE_FLUSHING,
    BZM_ENGINE_RESULT_PENDING,
    BZM_ENGINE_UNHEALTHY,
} bzm_engine_state_t;

typedef enum {
    BZM_ASSIGN_OK = 0,
    BZM_ASSIGN_BUSY,
    BZM_ASSIGN_FLUSH_REQUIRED,
    BZM_ASSIGN_STORE_ERROR,
    BZM_ASSIGN_TRANSPORT_ERROR,
    BZM_ASSIGN_INVALID,
} bzm_assign_status_t;

typedef struct {
    bool (*write_work)(void *context, const bzm_work_t *work);
    /* Optional batch-dispatch checkpoint after each complete logical-engine
     * write. Hardware transports use this to bound an otherwise continuous
     * command burst and drain full-duplex receive traffic. Returning false
     * aborts and symmetrically flushes the incomplete generation. */
    bool (*dispatch_checkpoint)(void *context);
    bool (*flush)(void *context);
} bzm_transport_ops_t;

typedef struct {
    uint16_t engine_count;
    uint8_t timestamp_count;
    uint8_t lead_zeros;
    uint32_t nonce_offset;
    bool enhanced_mode;
} bzm_reactor_config_t;

typedef struct {
    bool active;
    uint16_t logical_engine_id;
    uint16_t engine_id;
    bzm_engine_state_t state;
    uint8_t logical_sequence;
    uint32_t epoch;
    asic_work_handle_t handle;
    uint8_t timestamp_count;
    uint32_t base_ntime;
    uint32_t base_version;
    uint32_t nonce_offset;
    bool enhanced_mode;
    uint8_t version_count;
    uint32_t versions[BZM_VERSION_VARIANTS];
} bzm_assignment_t;

typedef struct {
    asic_job_store_t *job_store;
    bzm_transport_ops_t transport;
    void *transport_context;
    bzm_reactor_config_t config;
    bzm_assignment_t assignments[BZM_MAX_ACTIVE_WORK];
    /* The hardware may report the just-replaced job after the next job has
     * been programmed. Retain one prior generation independently per engine
     * instead of assuming every engine shares one batch identity. */
    bzm_assignment_t previous_assignments[BZM_MAX_ACTIVE_WORK];
    uint8_t next_engine_sequence[BZM_MAX_ACTIVE_WORK];
    /* A completed full dispatch has common job/version/time metadata across
     * all engines. Retain that compact descriptor while the next sequence is
     * programmed so in-flight results from both hardware generations map. */
    bzm_assignment_t previous_batch;
    bool previous_batch_complete;
    bool current_batch_complete;
    uint16_t next_engine;
    uint16_t next_sequence;
    uint32_t epoch;
    bool flush_pending;
    bool flush_complete;
    /* Clean jobs invalidate every old pool handle. Results are deliberately
     * ignored until all engines have received the replacement generation,
     * matching the BIRDS flush boundary. */
    bool results_quarantined;
} bzm_reactor_t;

bool bzm_reactor_init(bzm_reactor_t *reactor, asic_job_store_t *job_store,
                      const bzm_reactor_config_t *config,
                      const bzm_transport_ops_t *transport,
                      void *transport_context);

bzm_assign_status_t bzm_reactor_assign(bzm_reactor_t *reactor,
                                       const mining_template_t *template,
                                       bzm_work_t *assigned_work);

// Distribute one stored upstream template to every configured engine. All
// engine assignments share one generation-bearing handle.
bzm_assign_status_t bzm_reactor_dispatch(bzm_reactor_t *reactor,
                                         const mining_template_t *template,
                                         size_t *assigned_count);

bool bzm_reactor_begin_flush(bzm_reactor_t *reactor);
void bzm_reactor_finish_flush(bzm_reactor_t *reactor);
bool bzm_reactor_is_flush_pending(const bzm_reactor_t *reactor);
bool bzm_reactor_results_quarantined(const bzm_reactor_t *reactor);
// Complete one clean-job barrier and retire every prior assignment/handle.
// An idle reactor invalidates the store without emitting hardware flush work.
bool bzm_reactor_clear_work(bzm_reactor_t *reactor);

bool bzm_reactor_map_result(bzm_reactor_t *reactor,
                            const bzm_raw_result_t *raw,
                            asic_event_t *event);

#endif // BZM_REACTOR_H
