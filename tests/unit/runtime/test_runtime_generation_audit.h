/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_generation_audit.h - Independent lifecycle audit gate entry
 */

#ifndef TEST_RUNTIME_GENERATION_AUDIT_H
#define TEST_RUNTIME_GENERATION_AUDIT_H

typedef struct XrTargetPlan XrTargetPlan;

/* Re-checks a real generation lifecycle against the independent state-machine
 * verifier and rejects hostile mutations of the snapshots it produced. The
 * caller owns the verified sole-function scalar-i64 plan. */
void run_generation_lifecycle_audit(const XrTargetPlan *plan);

#endif  // TEST_RUNTIME_GENERATION_AUDIT_H
