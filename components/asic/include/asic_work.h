#ifndef ASIC_WORK_H
#define ASIC_WORK_H

#include "asic_result.h"
#include "mining.h"

typedef struct {
    asic_work_handle_t handle;
    const mining_template_t *template;
} asic_work_t;

#endif // ASIC_WORK_H
