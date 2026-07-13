#ifndef BM_JOB_BUILDER_H_
#define BM_JOB_BUILDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "bm_job.h"
#include "mining.h"

// Convert a family-neutral template into Bitmain packet material.
bool bm_job_build(const mining_template_t *template, bm_job *job);

#endif // BM_JOB_BUILDER_H_
