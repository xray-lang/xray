/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pending_exit_nested_fixture.h - Distinct LIFO cleanup observations
 */
#ifndef XIR_PENDING_EXIT_NESTED_FIXTURE_H
#define XIR_PENDING_EXIT_NESTED_FIXTURE_H
#include "xir_cleanup_recovery_fixture.h"
typedef struct PendingExitNestedFixture {
    CleanupRecoveryFixture base;
    XrXirInstruction inner[3];
    XrXirFunction functions[4];
    XrXirFunctionIdentity identities[4];
} PendingExitNestedFixture;
static void pending_exit_nested_fixture(PendingExitNestedFixture *f) {
    *f = (PendingExitNestedFixture){0};
    cleanup_recovery_fixture(&f->base, false, true);
    for (unsigned i = 0; i < 3; ++i) {
        f->functions[i] = f->base.functions[i];
        f->identities[i] = f->base.identities[i];
        f->inner[i] = f->base.cleanup[i];
    }
    f->inner[0].immediate = 42;
    f->functions[3] = f->functions[2];
    f->functions[3].name = "inner";
    f->functions[3].name_length = 5;
    f->functions[3].instructions = f->inner;
    f->identities[3].cleanup_owner = 2;
    f->base.root[4].immediate = 3;
    f->base.declarations.functions = f->identities;
    f->base.module.functions = f->functions;
    f->base.module.function_count = 4;
}
#endif
