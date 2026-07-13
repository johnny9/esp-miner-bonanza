#ifndef BZM_DRIVER_H
#define BZM_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "asic_result.h"
#include "mining.h"

typedef struct GlobalState GlobalState;

uint8_t BZM_init(GlobalState *state);
int BZM_set_max_baud(void);
bool BZM_send_work(GlobalState *state, const mining_template_t *template);
asic_event_t *BZM_process_work(GlobalState *state);
float BZM_read_temperature(GlobalState *state);

#endif // BZM_DRIVER_H
