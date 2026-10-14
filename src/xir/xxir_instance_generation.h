/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_generation.h - Owned candidate code and storage publication
 *
 * KEY CONCEPT:
 *   One Instance owns the cumulative Domain and Call ledger. Each immutable
 *   Program keeps its own arena, permissions and initialized storage.
 */
#ifndef XXIR_INSTANCE_GENERATION_H
#define XXIR_INSTANCE_GENERATION_H
#include "xxir_program.h"

/* Creates an empty owner before any input is compiled or executed. Failure
 * preserves an empty output; every later candidate spends this owner's ledger. */
XR_FUNC XrXirCallStatus xr_xir_instance_generation_new(const XrXirInstanceConfig *config,
    XrXirInstance **output);
/* Borrows an already sealed Program and retains it on success. The nonzero
 * serial is never reused. Normal start/poll/resume drive one candidate Call;
 * after that Call exists it cannot be replaced before publish or abort. A
 * pending candidate excludes older function starts and another preparation.
 * Source declaration/import correspondence is not inferred from names. */
XR_FUNC XrXirCallStatus xr_xir_instance_generation_prepare(XrXirInstance *instance,
    XrXirProgram *program, uint64_t *generation);
/* Publishes only an idle, normally returned candidate. Commit allocates
 * nothing. Previous code/storage remain owned and old functions keep their
 * exact birth Program; no permission or type-arena identity is rewritten. */
XR_FUNC XrXirCallStatus xr_xir_instance_generation_publish(XrXirInstance *instance,
    uint64_t generation);
/* Consumes the matching candidate except on BUSY. Restores the previous Call;
 * old object side effects and cumulative charges are never rolled back.
 * Drained cleanup/runtime failure is returned unchanged. */
XR_FUNC XrXirCallStatus xr_xir_instance_generation_abort(XrXirInstance *instance,
    uint64_t generation);
XR_FUNC XrXirCallStatus xr_xir_instance_generation_published(const XrXirInstance *instance,
    uint64_t *generation);
#endif // XXIR_INSTANCE_GENERATION_H
