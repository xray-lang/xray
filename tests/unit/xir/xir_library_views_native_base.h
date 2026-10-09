/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_views_native_base.h - Authentic generated bodies and code leases
 *
 * KEY CONCEPT:
 *   Authentic bodies preserve their active view while observations count edges.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include "xir/xxir_vm.h"
#ifdef H1_MIXED
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"H1 native/mixed %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "xir_library_views_native_oracles.h"
#include "library_views_native_generated.h"

typedef struct H1NativeOwner H1NativeOwner;
typedef struct H1NativeBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    H1NativeOwner *owner;
    uint32_t function;
    bool native;
} H1NativeBinding;
struct H1NativeOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry *entries;
    H1NativeBinding *bindings;
    uint32_t count;
    unsigned code_releases;
    uint32_t run, entry;
    uint64_t run_releases[2], entry_releases[2];
    uintptr_t instance_ids[2]; /* Sampled during a live provider callback. */
    uint64_t steps[2][2], releases[2][2], crossings[2];
};

static unsigned h1n_instance(H1NativeOwner *owner, XrXirInstance *instance) {
    CHECK(owner && instance);
    const uintptr_t identity = (uintptr_t)instance;
    for (unsigned i = 0; i < 2; ++i) {
        if (owner->instance_ids[i] == identity) return i;
        if (!owner->instance_ids[i]) { owner->instance_ids[i] = identity; return i; }
    }
    CHECK(false); return 0;
}

static XrXirAction h1n_resume(XrXirCallView *view) {
    CHECK(view && view->environment && xr_xir_call_admission(view));
    H1NativeBinding *binding = (H1NativeBinding *)view->environment;
    H1NativeOwner *owner = binding->owner;
    CHECK(owner && binding == &owner->bindings[binding->function] && !owner->code_releases);
    unsigned instance = h1n_instance(owner,view->instance);
    ++owner->steps[instance][binding->native];
    const void *environment = view->environment;
    XrXirAction action = binding->actual.resume(view);
    CHECK(view->environment == environment && xr_xir_call_admission(view));
    if (action.kind == XR_XIR_ACTION_CALL) {
        CHECK(action.callee < owner->count);
        if (binding->native != owner->bindings[action.callee].native) ++owner->crossings[instance];
    }
    return action;
}

static void h1n_release(XrXirCallView *view, XrXirCallStatus reason) {
    CHECK(view && view->environment && !xr_xir_call_admission(view));
    H1NativeBinding *binding = (H1NativeBinding *)view->environment;
    H1NativeOwner *owner = binding->owner;
    unsigned instance = h1n_instance(owner,view->instance);
    ++owner->releases[instance][binding->native];
    if(binding->function==owner->run)++owner->run_releases[instance];
    if(binding->function==owner->entry)++owner->entry_releases[instance];
    const void *environment = view->environment;
    binding->actual.release(view,reason);
    CHECK(view->environment == environment);
}

static void h1n_code_drop(void *opaque) {
    H1NativeOwner *owner = opaque;
    CHECK(owner && !owner->code_releases && owner->entries && owner->bindings);
    xr_xir_compile_artifact_free(owner->lowered); owner->lowered = NULL;
    xr_compile_resources_free(owner->entries); owner->entries = NULL;
    xr_compile_resources_free(owner->bindings); owner->bindings = NULL;
    ++owner->code_releases;
}
