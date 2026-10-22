/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe.c - Independent finite compiler instruction execution
 *
 * KEY CONCEPT:
 *   Only the common scalar kernel is linked; no native-cache bootstrap dependency.
 */
#include "xxir_ctfe.h"
#include "xxir_intrinsic.h"
#include "xxir_ctfe_batch_internal.h"
#include "xxir_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_types.h"
#include "xxir_interpreter_state.h"
#include "xxir_scalar_core.h"
#include <string.h>
typedef XirScalarCoreRun ScalarRun;
static bool scalar_compile_work(ScalarRun *run,uint64_t units) {
    return xir_scalar_core_work(run,units);
}
#include "xxir_ctfe.inc.c"
