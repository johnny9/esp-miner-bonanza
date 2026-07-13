#include "bzm_reactor.h"

#include <string.h>

static uint16_t sequence_space(const bzm_reactor_t *reactor)
{
    return reactor->config.enhanced_mode ? 2 : 256;
}

static void partition_nonce_space(const bzm_reactor_t *reactor,
                                  uint16_t logical_engine_id,
                                  bzm_work_t *work)
{
    uint64_t total = (uint64_t)UINT32_MAX + 1;
    uint64_t step = total / reactor->config.engine_count;
    uint64_t start = step * logical_engine_id;
    uint64_t end = logical_engine_id + 1 == reactor->config.engine_count
        ? UINT32_MAX : (step * (logical_engine_id + 1)) - 1;
    work->starting_nonce = (uint32_t)start;
    work->end_nonce = (uint32_t)end;
}

static bzm_assignment_t *find_assignment(bzm_reactor_t *reactor,
                                         uint16_t engine_id)
{
    for (size_t i = 0; i < BZM_MAX_ACTIVE_WORK; ++i) {
        bzm_assignment_t *assignment = &reactor->assignments[i];
        if (assignment->active && assignment->engine_id == engine_id) {
            return assignment;
        }
    }
    return NULL;
}

static bzm_assignment_t *free_assignment(bzm_reactor_t *reactor)
{
    for (size_t i = 0; i < BZM_MAX_ACTIVE_WORK; ++i) {
        bzm_assignment_t *assignment = &reactor->assignments[i];
        if (!assignment->active || assignment->state == BZM_ENGINE_IDLE) {
            return assignment;
        }
    }
    return NULL;
}

bool bzm_reactor_init(bzm_reactor_t *reactor, asic_job_store_t *job_store,
                      const bzm_reactor_config_t *config,
                      const bzm_transport_ops_t *transport,
                      void *transport_context)
{
    if (reactor == NULL || job_store == NULL || config == NULL ||
        transport == NULL || transport->write_work == NULL ||
        config->engine_count == 0 ||
        config->engine_count > BZM_ENGINES_PER_ASIC ||
        config->timestamp_count > 0x7f) {
        return false;
    }

    memset(reactor, 0, sizeof(*reactor));
    reactor->job_store = job_store;
    reactor->config = *config;
    reactor->transport = *transport;
    reactor->transport_context = transport_context;
    reactor->epoch = 1;
    return true;
}

bool bzm_reactor_begin_flush(bzm_reactor_t *reactor)
{
    if (reactor == NULL || reactor->job_store == NULL) return false;
    if (reactor->flush_pending) {
        if (reactor->flush_complete) return true;
        reactor->flush_complete = reactor->transport.flush == NULL ||
            reactor->transport.flush(reactor->transport_context);
        return reactor->flush_complete;
    }

    reactor->flush_pending = true;
    reactor->flush_complete = false;
    if (++reactor->epoch == 0) reactor->epoch = 1;
    asic_job_store_invalidate_all(reactor->job_store);
    for (size_t i = 0; i < BZM_MAX_ACTIVE_WORK; ++i) {
        if (reactor->assignments[i].active) {
            reactor->assignments[i].state = BZM_ENGINE_FLUSHING;
        }
    }

    reactor->flush_complete = reactor->transport.flush == NULL ||
        reactor->transport.flush(reactor->transport_context);
    return reactor->flush_complete;
}

void bzm_reactor_finish_flush(bzm_reactor_t *reactor)
{
    if (reactor == NULL || !reactor->flush_pending ||
        !reactor->flush_complete) {
        return;
    }
    memset(reactor->assignments, 0, sizeof(reactor->assignments));
    reactor->next_engine = 0;
    reactor->next_sequence = 0;
    reactor->flush_pending = false;
    reactor->flush_complete = false;
}

bool bzm_reactor_is_flush_pending(const bzm_reactor_t *reactor)
{
    return reactor != NULL && reactor->flush_pending;
}

bzm_assign_status_t bzm_reactor_assign(bzm_reactor_t *reactor,
                                       const mining_template_t *template,
                                       bzm_work_t *assigned_work)
{
    if (reactor == NULL || template == NULL || reactor->job_store == NULL) {
        return BZM_ASSIGN_INVALID;
    }
    if (reactor->flush_pending) return BZM_ASSIGN_FLUSH_REQUIRED;
    if (reactor->next_sequence >= sequence_space(reactor)) {
        return bzm_reactor_begin_flush(reactor)
            ? BZM_ASSIGN_FLUSH_REQUIRED : BZM_ASSIGN_TRANSPORT_ERROR;
    }

    uint16_t logical_engine_id = reactor->next_engine;
    uint16_t engine_id;
    if (!bzm_engine_physical_id(logical_engine_id, &engine_id)) {
        return BZM_ASSIGN_INVALID;
    }
    bzm_assignment_t *existing = find_assignment(reactor, engine_id);
    if (existing != NULL && existing->state != BZM_ENGINE_IDLE) {
        return BZM_ASSIGN_BUSY;
    }
    bzm_assignment_t *assignment = existing ? existing : free_assignment(reactor);
    if (assignment == NULL) return BZM_ASSIGN_BUSY;

    asic_work_handle_t handle = ASIC_WORK_HANDLE_INVALID;
    if (!asic_job_store_store_generated(reactor->job_store, template,
                                        &handle)) {
        return BZM_ASSIGN_STORE_ERROR;
    }

    asic_work_t source = {
        .handle = handle,
        .template = template,
    };
    bzm_work_t work;
    if (!bzm_work_build(&source, engine_id,
                        (uint8_t)reactor->next_sequence,
                        reactor->config.timestamp_count,
                        reactor->config.lead_zeros,
                        reactor->config.enhanced_mode, &work)) {
        asic_job_store_release(reactor->job_store, handle);
        return BZM_ASSIGN_INVALID;
    }
    partition_nonce_space(reactor, logical_engine_id, &work);
    if (!reactor->transport.write_work(reactor->transport_context, &work)) {
        asic_job_store_release(reactor->job_store, handle);
        return BZM_ASSIGN_TRANSPORT_ERROR;
    }

    *assignment = (bzm_assignment_t) {
        .active = true,
        .logical_engine_id = logical_engine_id,
        .engine_id = engine_id,
        .state = BZM_ENGINE_ASSIGNED,
        .logical_sequence = (uint8_t)reactor->next_sequence,
        .epoch = reactor->epoch,
        .handle = handle,
        .timestamp_count = work.timestamp_count,
        .base_ntime = work.start_ntime,
        .base_version = template->version,
        .nonce_offset = reactor->config.nonce_offset,
        .enhanced_mode = work.midstate_count > 1,
        .version_count = work.midstate_count,
    };
    memcpy(assignment->versions, work.versions,
           sizeof(assignment->versions));

    reactor->next_engine =
        (logical_engine_id + 1) % reactor->config.engine_count;
    reactor->next_sequence++;
    if (assigned_work != NULL) *assigned_work = work;
    return BZM_ASSIGN_OK;
}

bzm_assign_status_t bzm_reactor_dispatch(bzm_reactor_t *reactor,
                                         const mining_template_t *template,
                                         size_t *assigned_count)
{
    if (assigned_count != NULL) *assigned_count = 0;
    if (reactor == NULL || template == NULL || reactor->job_store == NULL) {
        return BZM_ASSIGN_INVALID;
    }
    if (reactor->flush_pending) return BZM_ASSIGN_FLUSH_REQUIRED;
    if (reactor->next_sequence >= sequence_space(reactor)) {
        return bzm_reactor_begin_flush(reactor)
            ? BZM_ASSIGN_FLUSH_REQUIRED : BZM_ASSIGN_TRANSPORT_ERROR;
    }
    if (reactor->config.engine_count > BZM_MAX_ACTIVE_WORK) {
        return BZM_ASSIGN_INVALID;
    }

    asic_work_handle_t handle = ASIC_WORK_HANDLE_INVALID;
    if (!asic_job_store_store_generated(reactor->job_store, template,
                                        &handle)) {
        return BZM_ASSIGN_STORE_ERROR;
    }
    asic_work_t source = {
        .handle = handle,
        .template = template,
    };

    size_t completed = 0;
    for (uint16_t logical_engine_id = 0;
         logical_engine_id < reactor->config.engine_count;
         ++logical_engine_id) {
        uint16_t engine_id;
        if (!bzm_engine_physical_id(logical_engine_id, &engine_id)) break;
        bzm_assignment_t *assignment = find_assignment(reactor, engine_id);
        if (assignment == NULL) assignment = free_assignment(reactor);
        if (assignment == NULL) break;

        bzm_work_t work;
        if (!bzm_work_build(&source, engine_id,
                            (uint8_t)reactor->next_sequence,
                            reactor->config.timestamp_count,
                            reactor->config.lead_zeros,
                            reactor->config.enhanced_mode, &work)) {
            break;
        }
        partition_nonce_space(reactor, logical_engine_id, &work);
        if (!reactor->transport.write_work(reactor->transport_context,
                                           &work)) {
            break;
        }

        *assignment = (bzm_assignment_t) {
            .active = true,
            .logical_engine_id = logical_engine_id,
            .engine_id = engine_id,
            .state = BZM_ENGINE_ASSIGNED,
            .logical_sequence = (uint8_t)reactor->next_sequence,
            .epoch = reactor->epoch,
            .handle = handle,
            .timestamp_count = work.timestamp_count,
            .base_ntime = work.start_ntime,
            .base_version = template->version,
            .nonce_offset = reactor->config.nonce_offset,
            .enhanced_mode = work.midstate_count > 1,
            .version_count = work.midstate_count,
        };
        memcpy(assignment->versions, work.versions,
               sizeof(assignment->versions));
        completed++;
    }

    if (completed == 0) {
        asic_job_store_release(reactor->job_store, handle);
        return BZM_ASSIGN_TRANSPORT_ERROR;
    }
    if (completed != reactor->config.engine_count) {
        bool flushed = bzm_reactor_begin_flush(reactor);
        if (flushed) bzm_reactor_finish_flush(reactor);
        if (assigned_count != NULL) *assigned_count = 0;
        return BZM_ASSIGN_TRANSPORT_ERROR;
    }
    reactor->next_engine = 0;
    reactor->next_sequence++;
    if (assigned_count != NULL) *assigned_count = completed;
    return BZM_ASSIGN_OK;
}

bool bzm_reactor_map_result(bzm_reactor_t *reactor,
                            const bzm_raw_result_t *raw,
                            asic_event_t *event)
{
    uint16_t logical_engine_id;
    if (reactor == NULL || raw == NULL || event == NULL ||
        reactor->flush_pending ||
        !bzm_engine_logical_id(raw->engine_id, &logical_engine_id) ||
        logical_engine_id >= reactor->config.engine_count ||
        (raw->status & 0x8) == 0) {
        return false;
    }

    bzm_assignment_t *assignment = find_assignment(reactor, raw->engine_id);
    if (assignment == NULL ||
        assignment->logical_engine_id != logical_engine_id ||
        assignment->state != BZM_ENGINE_ASSIGNED ||
        assignment->epoch != reactor->epoch) {
        return false;
    }

    uint8_t logical_sequence = raw->sequence_id;
    uint8_t micro_job = 0;
    if (assignment->enhanced_mode) {
        logical_sequence >>= 2;
        micro_job = raw->sequence_id & 0x03;
    }
    if (logical_sequence != assignment->logical_sequence ||
        micro_job >= assignment->version_count ||
        raw->time > assignment->timestamp_count) {
        return false;
    }

    uint32_t final_version = assignment->versions[micro_job];
    uint32_t ntime_offset = assignment->timestamp_count - raw->time;
    *event = (asic_event_t) {
        .type = ASIC_EVENT_SHARE_RESULT,
        .data.share = {
            .work_handle = assignment->handle,
            .nonce = raw->nonce - assignment->nonce_offset,
            .final_ntime = assignment->base_ntime + ntime_offset,
            .final_version = final_version,
            .version_bits = final_version ^ assignment->base_version,
            .timestamp_us = raw->timestamp_us,
            .engine_id = assignment->logical_engine_id,
            .sequence_id = assignment->logical_sequence,
            .micro_job_id = micro_job,
            .generation = assignment->epoch,
        },
    };
    return true;
}
