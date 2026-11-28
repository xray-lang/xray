/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_owner_elide.h - Copy bindings rebuilt as owner moves and borrows
 */
#ifndef XLIR_OWNER_ELIDE_H
#define XLIR_OWNER_ELIDE_H
#include "xlir.h"

/* Rebuild a readable module with redundant reference pairs of identity-free
 * owners (xr_lir_identity_free_owner) removed.
 * A retain shares its source's single reference when whichever of the two
 * owners retires first does so through a plain drop; that drop and the retain
 * disappear. Incoming parameters stay valid for the whole call, so a copy that
 * is only dropped borrows them. Copies consumed by an owned append, a result
 * or an edge while their source is still owned are kept. Function, block and
 * edge ordinals and the complete registry are preserved; value IDs may change.
 * The output is verified before publication. Output must be empty; failure publishes nothing
 * and preserves input and any occupied output. */
XR_FUNC XrLirStatus xr_lir_owner_elide(const XrLirModule *input, XrLirModule **output);
#endif // XLIR_OWNER_ELIDE_H
