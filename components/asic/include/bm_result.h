#ifndef BM_RESULT_H_
#define BM_RESULT_H_

#include <stdbool.h>
#include <stdint.h>

#include "asic_common.h"

bool bm_result_to_event(const task_result *result, asic_event_t *event);

#endif // BM_RESULT_H_
