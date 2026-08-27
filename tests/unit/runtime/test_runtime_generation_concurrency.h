/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_generation_concurrency.h - Generation lifecycle race gate entry
 */

#ifndef TEST_RUNTIME_GENERATION_CONCURRENCY_H
#define TEST_RUNTIME_GENERATION_CONCURRENCY_H

typedef struct XrTargetPlan XrTargetPlan;

/* Runs every lifecycle race gate against one verified sole-function scalar-i64
 * plan that returns 42. The caller owns the plan and outlives the call. */
void run_generation_lifecycle_races(const XrTargetPlan *plan);

#endif  // TEST_RUNTIME_GENERATION_CONCURRENCY_H
