#ifndef ASIC_RESET_H_
#define ASIC_RESET_H_

#include "esp_err.h"
#include "global_state.h"

esp_err_t asic_reset(GlobalState *state);
esp_err_t asic_hold_reset_low(GlobalState *state);

#endif /* ASIC_RESET_H_ */
