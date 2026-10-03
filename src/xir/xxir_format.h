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

/* Nodes borrow actual owned values. Every parent must remain alive throughout
 * traversal; returned child pointers and metadata have that same lifetime. */
XR_FUNC XrValueFormatReader xr_xir_value_format_reader(void);
#endif // XXIR_FORMAT_H
