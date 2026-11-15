/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_place.h - Internal support for the public ownership transaction ABI
 *
 * KEY CONCEPT:
 *   A place borrows storage, not a reference or mutation capability. Every
 *   possible rejection precedes publication; retirement runs while both storage
 *   domains still reject reentrant ownership changes.
 */
#ifndef XNATIVE_PLACE_H
#define XNATIVE_PLACE_H
#include "xnative_managed_root.h"
#include "xray_native_place.h"
#endif // XNATIVE_PLACE_H
