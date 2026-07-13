#ifndef SV2_MINING_TEMPLATE_H_
#define SV2_MINING_TEMPLATE_H_

#include <stdbool.h>
#include <stdint.h>

#include "mining.h"
#include "sv2_protocol.h"

bool mining_template_build_sv2_standard(const sv2_job_t *source,
                                        uint32_t version_mask,
                                        double difficulty,
                                        mining_template_t *template);

bool mining_template_build_sv2_extended(const sv2_ext_job_t *source,
                                        const sv2_conn_t *connection,
                                        uint64_t extranonce2_counter,
                                        uint32_t version_mask,
                                        double difficulty,
                                        mining_template_t *template);

#endif // SV2_MINING_TEMPLATE_H_
