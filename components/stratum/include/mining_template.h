#ifndef MINING_TEMPLATE_H_
#define MINING_TEMPLATE_H_

#include <stdbool.h>
#include <stdint.h>

#include "mining.h"
#include "stratum_api.h"

bool mining_template_build_sv1(const mining_notify *notification,
                               const char *extranonce_prefix,
                               uint32_t extranonce2_len,
                               uint64_t extranonce2_counter,
                               uint32_t version_mask, double difficulty,
                               mining_template_t *template);

#endif // MINING_TEMPLATE_H_
