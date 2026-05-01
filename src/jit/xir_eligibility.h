/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_eligibility.h - Decide whether a bytecode proto can be JIT-compiled.
 *
 * KEY CONCEPT:
 *   Centralises *all* entry gates that determine whether a given XrProto
 *   is a valid candidate for JIT translation, so both the main compile
 *   driver (xir_jit.c) and the background compile queue
 *   (xjit_compile_queue.c) share exactly the same criteria.
 *
 * WHAT THIS FILE COVERS:
 *   - Function size / complexity limits derived from the current target
 *   - Parameter and upvalue count limits
 *   - Deopt backoff retry policy
 *   - Parameter / return type slot_type eligibility
 *
 * WHAT IT DOES NOT COVER (handled elsewhere):
 *   - Actual IR construction (xi_to_xir.c)
 *   - Register allocation / codegen refusal (xir_regalloc.c / xir_codegen.c)
 *   - Background queue scheduling / task snapshotting (xjit_compile_queue.c)
 */

#ifndef XIR_ELIGIBILITY_H
#define XIR_ELIGIBILITY_H

#include <stdbool.h>
#include "../base/xdefs.h"

struct XrProto;

/*
 * Return true iff |proto| can be JIT-compiled under the current target.
 *
 * This function performs *only* read-only checks on |proto| and global
 * target state, so it is safe to call from the background compile worker
 * as well as the main thread.  When |verbose| is true, detailed reasons
 * for rejection are logged to stderr to aid JIT tuning.
 */
XR_FUNC bool is_jit_eligible(struct XrProto *proto, bool verbose);

#endif  // XIR_ELIGIBILITY_H
