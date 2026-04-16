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

#include <stddef.h>
#include <stdint.h>
#include "../base/xdefs.h"

// Get pre-compiled bytecode for a stdlib module.
// Returns NULL if module not found or no bytecode available.
XR_FUNC const uint8_t* xr_get_embedded_stdlib_bytecode(const char *module_name, size_t *out_size);

// Get source code for a stdlib module (fallback).
// Returns NULL if module not found.
XR_FUNC const char* xr_get_embedded_stdlib(const char *module_name);

#endif
