/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_rebuild.h - Independent ownership through immutable SSA views
 */
#ifndef XLIR_REBUILD_H
#define XLIR_REBUILD_H
#include "xlir.h"

/* Verify a readable input once and copy its entire module through the public
 * builder. Function/block/edge ordinals and all registry entries are preserved;
 * value IDs may change. Output must be empty and is published only on success.
 * Failure preserves input and any occupied output. No input storage is retained.
 * Finished empty and external-only modules are supported; this selects no root. */
XR_FUNC XrLirStatus xr_lir_rebuild(const XrLirModule *input, XrLirModule **output);

#endif // XLIR_REBUILD_H
