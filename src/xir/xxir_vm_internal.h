/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_internal.h - Verified native selection before immutable sealing
 *
 * KEY CONCEPT: Native cache hits and Lowered misses share one Program owner.
 */
#ifndef XXIR_VM_INTERNAL_H
#define XXIR_VM_INTERNAL_H
#include "xxir_vm.h"
typedef struct XirNativeCache XirNativeCache;
/* Cache and artifact must share the receiving resource owner. Success takes
 * and clears the artifact while retaining the cache. Failure preserves inputs
 * and the output. Ordinary instances are complete before Program sealing. */
XR_FUNC XrXirStatus xir_compile_vm_program_take_cached(XrXirArtifact **artifact,
    XirNativeCache *cache, XrXirProgram **output);
#endif // XXIR_VM_INTERNAL_H
