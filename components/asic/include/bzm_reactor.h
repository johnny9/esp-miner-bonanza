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
    uint16_t next_engine;
    uint16_t next_sequence;
    uint32_t epoch;
    bool flush_pending;
    bool flush_complete;
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

bool bzm_reactor_map_result(bzm_reactor_t *reactor,
                            const bzm_raw_result_t *raw,
                            asic_event_t *event);

#endif // BZM_REACTOR_H
