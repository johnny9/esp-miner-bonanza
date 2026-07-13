#include "asic_job_store.h"

#include <string.h>

static void clear_entry(asic_job_store_entry_t *entry)
{
    if (entry == NULL) return;
    if (entry->valid) mining_template_free(&entry->template);
    memset(entry, 0, sizeof(*entry));
}

bool asic_job_store_init(asic_job_store_t *store)
{
    if (store == NULL) return false;
    memset(store, 0, sizeof(*store));
    store->next_generation = 1;
    return pthread_mutex_init(&store->lock, NULL) == 0;
}

void asic_job_store_destroy(asic_job_store_t *store)
{
    if (store == NULL) return;
    asic_job_store_invalidate_all(store);
    pthread_mutex_destroy(&store->lock);
    memset(store, 0, sizeof(*store));
}

static bool store_entry(asic_job_store_t *store, uint8_t slot,
                        asic_work_handle_t handle,
                        const mining_template_t *template)
{
    asic_job_store_entry_t replacement = {
        .valid = true,
        .handle = handle,
    };
    if (!mining_template_clone(template, &replacement.template)) return false;

    clear_entry(&store->entries[slot]);
    store->entries[slot] = replacement;
    return true;
}

bool asic_job_store_store_slot(asic_job_store_t *store, uint8_t slot,
                               const mining_template_t *template,
                               asic_work_handle_t *handle)
{
    if (store == NULL || template == NULL ||
        slot >= ASIC_JOB_STORE_CAPACITY) {
        return false;
    }

    pthread_mutex_lock(&store->lock);
    bool stored = store_entry(store, slot, slot, template);
    pthread_mutex_unlock(&store->lock);
    if (stored && handle != NULL) *handle = slot;
    return stored;
}

bool asic_job_store_store_generated(asic_job_store_t *store,
                                    const mining_template_t *template,
                                    asic_work_handle_t *handle)
{
    if (store == NULL || template == NULL) return false;

    pthread_mutex_lock(&store->lock);
    uint8_t slot = store->next_slot++ % ASIC_JOB_STORE_CAPACITY;
    uint64_t generation = store->next_generation++;
    if (store->next_generation == 0) store->next_generation = 1;
    asic_work_handle_t generated = (generation << 8) | slot;
    bool stored = store_entry(store, slot, generated, template);
    pthread_mutex_unlock(&store->lock);
    if (stored && handle != NULL) *handle = generated;
    return stored;
}

bool asic_job_store_snapshot(asic_job_store_t *store,
                             asic_work_handle_t handle,
                             mining_template_t *snapshot)
{
    if (store == NULL || snapshot == NULL ||
        handle == ASIC_WORK_HANDLE_INVALID) {
        return false;
    }

    uint8_t slot = (uint8_t)(handle & 0xff);
    if (slot >= ASIC_JOB_STORE_CAPACITY) return false;

    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&store->lock);
    asic_job_store_entry_t *entry = &store->entries[slot];
    bool found = entry->valid && entry->handle == handle &&
                 mining_template_clone(&entry->template, snapshot);
    pthread_mutex_unlock(&store->lock);
    return found;
}

bool asic_job_store_release(asic_job_store_t *store,
                            asic_work_handle_t handle)
{
    if (store == NULL || handle == ASIC_WORK_HANDLE_INVALID) return false;
    uint8_t slot = (uint8_t)(handle & 0xff);
    if (slot >= ASIC_JOB_STORE_CAPACITY) return false;

    pthread_mutex_lock(&store->lock);
    asic_job_store_entry_t *entry = &store->entries[slot];
    bool released = entry->valid && entry->handle == handle;
    if (released) clear_entry(entry);
    pthread_mutex_unlock(&store->lock);
    return released;
}

void asic_job_store_invalidate_all(asic_job_store_t *store)
{
    if (store == NULL) return;
    pthread_mutex_lock(&store->lock);
    for (size_t i = 0; i < ASIC_JOB_STORE_CAPACITY; ++i) {
        clear_entry(&store->entries[i]);
    }
    store->next_slot = 0;
    if (++store->next_generation == 0) store->next_generation = 1;
    pthread_mutex_unlock(&store->lock);
}
