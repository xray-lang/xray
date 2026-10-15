/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_binding.h - Published symbols bound to exact runtime storage
 *
 * KEY CONCEPT:
 *   A binding keeps its birth Program and physical declaration. Lookup keys
 *   select declarations; they never substitute for execution or type authority.
 */
#ifndef XXIR_INSTANCE_BINDING_H
#define XXIR_INSTANCE_BINDING_H
#include "xxir_instance_generation.h"

typedef struct XrXirInstanceBinding XrXirInstanceBinding;
typedef enum XrXirInstanceBindingKind {
    XR_XIR_INSTANCE_BINDING_SLOT=1, XR_XIR_INSTANCE_BINDING_FUNCTION=2
} XrXirInstanceBindingKind;
typedef struct XrXirInstanceBindingSpec {
    const char *symbol;
    uint32_t length, kind, ordinal;
} XrXirInstanceBindingSpec;
typedef struct XrXirInstanceBindingView {
    uint64_t generation;
    const XrXirProgram *program;
    const XrXirTypeArena *arena;
    const XrXirTypes *types;
    const char *symbol;
    uint32_t length, kind, ordinal;
    XrXirType slot_type;
    uint32_t mutable;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
} XrXirInstanceBindingView;

/* Copies one candidate's complete physical declaration map before its first
 * Call. Inputs require caller exclusion through return. Failure preserves the
 * candidate and previously published bindings; cumulative work is not refunded.
 * The Source coordinator supplies lexical keys and enforces source visibility. */
XR_FUNC XrXirCallStatus xr_xir_instance_generation_bind(XrXirInstance *instance,
    uint64_t generation, const XrXirInstanceBindingSpec *bindings, uint32_t count);
/* Exact byte keys select the newest published definition. Returned handles and
 * metadata borrow the open Instance; abort never exposes a candidate's handles.
 * Old handles retain their birth identity after another definition shadows a key. */
XR_FUNC XrXirCallStatus xr_xir_instance_binding_lookup(XrXirInstance *instance,
    const char *symbol, uint32_t length, const XrXirInstanceBinding **output);
XR_FUNC XrXirCallStatus xr_xir_instance_binding_view(XrXirInstance *instance,
    const XrXirInstanceBinding *binding, XrXirInstanceBindingView *output);
/* Starts the exact published birth entry through the ordinary root driver.
 * A pending candidate excludes this external start. Arguments retain their real
 * arena identity; this entry performs no cross-arena retagging or conversion. */
XR_FUNC XrXirCallStatus xr_xir_instance_binding_start(XrXirInstance *instance,
    const XrXirInstanceBinding *binding, const XrXirValue *arguments, uint32_t count);
/* Resolves the real birth slot only within its authentic birth execution view.
 * Existing module, worker, publication and loan checks remain authoritative.
 * Metadata lookup does not authorize access from a different Program. */
XR_FUNC XrXirCallStatus xr_xir_instance_binding_slot_read(XrXirCallView *view,
    const XrXirInstanceBinding *binding, XrXirValue *output);
#endif // XXIR_INSTANCE_BINDING_H
