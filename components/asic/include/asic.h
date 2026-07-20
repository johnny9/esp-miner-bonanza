#ifndef ASIC_H
#define ASIC_H

#include <esp_err.h>
#include "global_state.h"
#include "asic_common.h"
#include "asic_capabilities.h"
#include "mining.h"

asic_capabilities_t ASIC_get_capabilities(const GlobalState *GLOBAL_STATE);

uint8_t ASIC_init(GlobalState * GLOBAL_STATE);
asic_event_t * ASIC_process_work(GlobalState * GLOBAL_STATE);
int ASIC_set_max_baud(GlobalState * GLOBAL_STATE);
bool ASIC_send_work(GlobalState *GLOBAL_STATE,
                    const mining_template_t *template);
// Apply a chip-specific clean-job barrier before shared work handles are
// invalidated. Drivers without an explicit barrier have no persistent
// hardware assignment state and therefore succeed as a no-op.
bool ASIC_clear_work(GlobalState *GLOBAL_STATE);
void ASIC_set_version_mask(GlobalState * GLOBAL_STATE, uint32_t mask);
void ASIC_set_frequency(GlobalState * GLOBAL_STATE);
void ASIC_set_nonce_space(GlobalState * GLOBAL_STATE);
double ASIC_get_asic_job_frequency_ms(GlobalState * GLOBAL_STATE);
void ASIC_read_registers(GlobalState * GLOBAL_STATE);
float ASIC_get_temperature(GlobalState * GLOBAL_STATE);

#endif // ASIC_H
