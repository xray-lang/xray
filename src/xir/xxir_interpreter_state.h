/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interpreter_state.h - Owned interpreter activation bookkeeping
 *
 * KEY CONCEPT:
 *   Compiler and runtime keep physical bookkeeping without sharing active frames.
 */
#ifndef XXIR_INTERPRETER_STATE_H
#define XXIR_INTERPRETER_STATE_H
#include "xxir_atomic.h"
#include "xxir_value_place.h"
#include "xxir_channel_call_internal.h"
typedef struct VmState {
    uint32_t instruction, destination, invoke, panic;
    uint32_t frontier, exit_target, exit_block, landing;
    XrXirType expected;
    bool initialized, waiting, cleanup_waiting, leaving;
    XrXirValue *arguments;
    XrXirValuePathStep *path_steps;
    XrXirAtomicProgress atomic;
    XrXirChannelCallState channel;
} VmState;
#endif // XXIR_INTERPRETER_STATE_H
