/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_generation_internal.h - Compiler-bound receiving of birth storage
 *
 * KEY CONCEPT:
 *   Only the actual Source receiving map binds these two sealed Programs.
 */
#ifndef XXIR_INSTANCE_GENERATION_INTERNAL_H
#define XXIR_INSTANCE_GENERATION_INTERNAL_H
#include "xxir_instance_generation.h"
typedef struct XirGenerationStorage {
    XrXirProgram *source, *target;
    const uint32_t *modules, *slots;
    uint32_t module_count, slot_count;
} XirGenerationStorage;
/* Private compiler consumer. The caller has derived exact Source-to-Program
 * code correspondence and owns every map array until this synchronous call
 * returns. Successful preparation copies routes on the actual runtime ledger. */
XR_FUNC XrXirCallStatus xir_instance_generation_prepare_received(XrXirInstance *instance,
    const XirGenerationStorage *storage, uint64_t *generation);
/* Borrowed actual root Domain. A retained Domain prevents allocator reuse from
 * turning a disposed runtime owner into a different Source session. */
XR_FUNC XrXirDomain *xir_instance_generation_domain(XrXirInstance *instance);
#endif // XXIR_INSTANCE_GENERATION_INTERNAL_H
