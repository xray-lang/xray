/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_embedded.h - Embedded stdlib lookup API
 *
 * KEY CONCEPT:
 *   Provides access to pre-compiled stdlib modules embedded as C arrays.
 *   Two lookup modes: bytecode (preferred) and source fallback.
 */

#ifndef XSTDLIB_EMBEDDED_H
#define XSTDLIB_EMBEDDED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../base/xdefs.h"

struct XrModule;
struct XrVMRuntime;

// Get pre-compiled bytecode for a stdlib module.
// Returns NULL if module not found or no bytecode available.
XR_FUNC const uint8_t *xr_get_embedded_stdlib_bytecode(const char *module_name, size_t *out_size);

// Get source code for a stdlib module (fallback).
// Returns NULL if module not found.
XR_FUNC const char *xr_get_embedded_stdlib(const char *module_name);

// Install generated private leaves for one embedded source module.
// Source-only modules without private leaves succeed without adding exports.
XR_FUNC bool xr_stdlib_embedded_private_leaves_install(struct XrVMRuntime *isolate,
                                                       struct XrModule *module,
                                                       const char *requested_module_name);

#endif
