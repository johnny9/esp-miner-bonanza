#ifndef BZM_BRIDGE_SWD_H
#define BZM_BRIDGE_SWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BZM_BRIDGE_FLASH_CAPACITY (2u * 1024u * 1024u)
#define BZM_BRIDGE_FLASH_PAGE_SIZE 256u
#define BZM_BRIDGE_FLASH_CHUNK_SIZE (16u * 1024u)

typedef enum
{
    BZM_BRIDGE_FLASH_PREPARING = 0,
    BZM_BRIDGE_FLASH_ERASING,
    BZM_BRIDGE_FLASH_PROGRAMMING,
    BZM_BRIDGE_FLASH_VERIFYING,
    BZM_BRIDGE_FLASH_RESETTING,
} bzm_bridge_flash_phase_t;

typedef struct
{
    size_t source_length;
    size_t program_length;
} bzm_bridge_flash_chunk_t;

typedef enum
{
    BZM_BRIDGE_STUB_POLL_WAIT = 0,
    BZM_BRIDGE_STUB_POLL_COMPLETE,
    BZM_BRIDGE_STUB_POLL_FAILED,
} bzm_bridge_stub_poll_result_t;

/* RP2040 SRAM flash-stub status/stage values used by the SWD poller. */
#define BZM_BRIDGE_STUB_STATUS_IDLE 0u
#define BZM_BRIDGE_STUB_STATUS_BUSY 1u
#define BZM_BRIDGE_STUB_STATUS_OK 2u
#define BZM_BRIDGE_STUB_STAGE_IDLE 0u
#define BZM_BRIDGE_STUB_STAGE_DONE 3u

typedef void (*bzm_bridge_flash_progress_fn)(void * context, bzm_bridge_flash_phase_t phase, size_t completed, size_t total);

bool bzm_bridge_flash_next_chunk(size_t image_size, size_t offset, bzm_bridge_flash_chunk_t * chunk);

bzm_bridge_stub_poll_result_t bzm_bridge_flash_stub_poll_result(bool observed_running, bool halted, uint32_t status,
                                                                uint32_t stage);

esp_err_t bzm_bridge_swd_flash(const uint8_t * image, size_t image_size, bzm_bridge_flash_progress_fn progress,
                               void * progress_context);

#endif /* BZM_BRIDGE_SWD_H */
