/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pending_exit_variants_fixture.h - Typed stream result and distinct LIFO
 */
#ifndef XIR_PENDING_EXIT_VARIANTS_FIXTURE_H
#define XIR_PENDING_EXIT_VARIANTS_FIXTURE_H
#include "xir_pending_exit_nested_fixture.h"
typedef struct PendingExitVariantFixture {
    PendingExitNestedFixture nested;
    XrXirInstruction stream[11];
    XrXirLiteral literal;
    XrXirBlock blocks[6];
    XrXirModule module;
} PendingExitVariantFixture;
static void pending_exit_variant_fixture(PendingExitVariantFixture *f, unsigned mode) {
    *f = (PendingExitVariantFixture){0};
    if (mode == 1) {
        pending_exit_nested_fixture(&f->nested);
        f->module = f->nested.base.module;
        return;
    }
    CleanupRecoveryFixture *base = &f->nested.base;
    cleanup_recovery_fixture(base, false, false);
    if (mode == 2) {
        f->stream[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
        f->stream[1] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .args = {0, 1}, .targets = {1}, .immediate = 2};
        f->stream[2] = (XrXirInstruction){.op = XR_XIR_CONST_STRING, .type = XR_XIR_STRING};
        f->stream[3] = (XrXirInstruction){.op = XR_XIR_WRITE_STREAM, .type = XR_XIR_BOOL, .args = {2}, .immediate = 1};
        f->stream[4] = (XrXirInstruction){.op = XR_XIR_BRANCH, .args = {3}, .targets = {3, 2}};
        f->stream[5] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
        f->stream[6] = (XrXirInstruction){.op = XR_XIR_CLEANUP_LEAVE, .targets = {4}};
        f->stream[7] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 70};
        f->stream[8] = (XrXirInstruction){.op = XR_XIR_CLEANUP_LEAVE, .targets = {5}};
        f->stream[9] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {5}};
        f->stream[10] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {7}};
        f->blocks[0] = (XrXirBlock){.count = 2};
        f->blocks[1] = (XrXirBlock){.first = 2, .count = 3, .frontier = 2};
        f->blocks[2] = (XrXirBlock){.first = 5, .count = 2, .frontier = 2};
        f->blocks[3] = (XrXirBlock){.first = 7, .count = 2, .frontier = 2};
        f->blocks[4] = (XrXirBlock){.first = 9, .count = 1};
        f->blocks[5] = (XrXirBlock){.first = 10, .count = 1};
        f->literal = (XrXirLiteral){"7", 1};
        base->declarations.literals = &f->literal;
        base->declarations.literal_count = 1;
        base->functions[1].instructions = f->stream;
        base->functions[1].instruction_count = 11;
        base->functions[1].blocks = f->blocks;
        base->functions[1].block_count = 6;
    }
    f->module = base->module;
}
#endif
