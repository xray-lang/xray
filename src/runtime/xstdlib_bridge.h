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

struct XrayIsolate;
struct XrRegex;
struct XrChannel;

/* ========== DateTime Bridge ========== */

// Format a DateTime object. Uses void* to avoid stdlib/datetime dependency.
XR_FUNC int xr_datetime_format(void *dt, const char *pattern, char *buf, size_t buf_size);

// Returns true iff v is a DateTime instance. Implemented in
// stdlib/datetime/datetime.c; declared here so the VM and the runtime
// formatter can probe without including stdlib headers.
XR_FUNC bool xr_value_is_datetime(struct XrayIsolate *X, XrValue v);

/* ========== Regex Bridge ========== */

// Get pattern string from regex object
XR_FUNC const char *xr_regex_pattern(const struct XrRegex *re);

// Extract XrRegex* from an XrValue
XR_FUNC struct XrRegex *xr_value_to_regex(XrValue v);

// Register the Regex XrClass with native body descriptor. Called from
// xr_prelude_register_all_native_types during isolate init.
XR_FUNC void xr_regex_register_class(struct XrayIsolate *isolate);

/*
 * Compile a regex literal (the OP_REGEX_COMPILE bytecode helper).
 * Both arguments must be strings; flag chars 'i' / 'm' / 's' are
 * recognized, anything else is silently ignored to mirror the old
 * inline parser. Returns a wrapped XrRegex value on success, or
 * xr_null() on parse / compile failure.
 *
 * Lives in stdlib/regex but is forward-declared here so the VM
 * dispatch loop can reach it without pulling stdlib headers into
 * src/vm — the same pattern xr_datetime_format / xr_value_to_regex
 * already use.
 */
XR_FUNC XrValue xr_regex_compile_literal(struct XrayIsolate *isolate, XrValue pattern,
                                         XrValue flags);

/* ========== Cluster Bridge ========== */

// Check if cluster mode is active
XR_FUNC bool xr_cluster_is_running(void);

// Find a named channel in the local cluster registry
XR_FUNC struct XrChannel *xr_cluster_find_channel_local(const char *name);

// Register a channel in the cluster registry
XR_FUNC void xr_cluster_register_channel(const char *name, struct XrChannel *ch);

#endif  // XSTDLIB_BRIDGE_H
