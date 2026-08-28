/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_bridge.h - Forward declarations for stdlib functions used by core VM
 *
 * KEY CONCEPT:
 *   Core VM code (src/) must not #include stdlib/ headers directly.
 *   This bridge header provides void*-typed forward declarations for
 *   stdlib functions that the VM needs to call.
 *
 * WHY THIS DESIGN:
 *   - Avoids circular dependency between src/ and stdlib/
 *   - Uses void* where stdlib-specific types would be needed
 *   - Centralizes all stdlib bridge declarations in one place
 */

#ifndef XSTDLIB_BRIDGE_H
#define XSTDLIB_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include "value/xvalue.h"

struct XrVMRuntime;
struct XrRegex;
struct XrChannel;

/* ========== Regex Bridge ========== */

/*
 * Build a Regex for a regex literal (the OP_REGEX_COMPILE bytecode helper).
 * Both arguments must be strings; flag chars 'i' / 'm' / 's' are recognised
 * and anything else is silently ignored, which is the long-standing behaviour
 * of xr_regex_core_parse_flags. It records the pattern and the flag mask;
 * compilation belongs to stdlib/regex/regex.xr and happens on first use.
 *
 * Lives in stdlib/regex but is forward-declared here so the VM dispatch loop
 * can reach it without pulling stdlib headers into src/vm.
 */
XR_FUNC XrValue xr_regex_compile_literal(struct XrVMRuntime *isolate, XrValue pattern,
                                         XrValue flags);

#endif  // XSTDLIB_BRIDGE_H
