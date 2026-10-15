/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_format.h - Borrowed typed values for the shared display algorithm
 *
 * KEY CONCEPT:
 *   The reader never retains a value or invents a second runtime layout.
 */
#ifndef XXIR_FORMAT_H
#define XXIR_FORMAT_H
#include "xxir_value.h"
#include "../shared/xr_value_format_core.h"

/* The root pins the arena and all backing storage throughout traversal.
 * Neither the root nor its children may be mutated or dropped during a read. */
XR_FUNC XrValueFormatReader xr_xir_value_format_reader(const XrXirValue *root);
#endif // XXIR_FORMAT_H
