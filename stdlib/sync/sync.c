/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * sync.c - generated binder host for the `sync` module
 *
 * KEY CONCEPT:
 *   `sync` is a pure-Xray stdlib module: Mutex/RwLock/Once/Barrier/Condvar are
 *   defined entirely in stdlib/sync/sync.xr as generic classes composing the
 *   builtin coroutine-aware Semaphore/CountdownLatch/Atomic primitives. There
 *   is no C implementation to bind here.
 *
 *   What is left is publishing the runtime-backed primitive classes under the
 *   module's own names. Those five names are declared as native_type_exports in
 *   stdlib/stdlib_boundary.toml, so this file holds no logic of its own: it
 *   only gives the generated binder a translation unit to live in.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/xisolate_api.h"

#define XR_STDLIB_VM_BIND_MODULE_SYNC 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_SYNC
