/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhash.c - Pure hash functions implementation (no XrValue dependency)
 */

#include "xhash.h"
#include "xchecks.h"

// All hash functions are now inline in xhash.h for hot path performance.
// This file kept as compilation unit to ensure header parses correctly.
