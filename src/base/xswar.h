/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xswar.h - SWAR (SIMD Within A Register) fast parsing
 *
 * KEY CONCEPT:
 *   Uses 64-bit registers to process 8 bytes in parallel.
 *   For fast integer/hex parsing and digit detection.
 *   Returns false on failure, caller can fallback to stdlib.
 */

#ifndef XSWAR_H
#define XSWAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xdefs.h"

XR_FUNC bool xr_swar_parse_uint(const char *s, size_t len, uint64_t *result);
XR_FUNC bool xr_swar_parse_int(const char *s, size_t len, int64_t *result);
XR_FUNC bool xr_swar_parse_hex(const char *s, size_t len, uint64_t *result);
XR_FUNC bool xr_swar_is_digits(const char *s, size_t len);
XR_FUNC bool xr_swar_is_8_digits(const char *s);
XR_FUNC uint64_t xr_swar_parse_8_digits(const char *s);

#endif  // XSWAR_H
