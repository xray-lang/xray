/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_state.h - Owned module storage and typed execution capabilities
 *
 * KEY CONCEPT:
 *   One immutable layout belongs to each stateful LIR module. A reference's
 *   payload and mutability survive SSA transport inside that layout identity.
 */
#ifndef XLIR_STATE_H
#define XLIR_STATE_H
#include "xlir.h"

typedef struct XrLirStateModule {
    uint32_t first_dependency, dependency_count;
} XrLirStateModule;

typedef struct XrLirStateSlot {
    uint32_t module;
    XrLirType payload;
    bool mutable;
    uint64_t offset;
    uint32_t size, alignment;
    XrLirType array_element;
} XrLirStateSlot;

typedef struct XrLirStateLayout {
    const XrLirStateModule *modules;
    uint32_t module_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    const XrLirStateSlot *slots;
    uint32_t slot_count;
    uint64_t payload_size;
    uint32_t payload_alignment;
} XrLirStateLayout;

typedef struct XrLirStateReference {
    XrLirType payload;
    bool mutable;
    XrLirType array_element;
} XrLirStateReference;

/* The builder copies every array. Module and slot ordinals are zero-based.
 * Slots are in ascending, nonoverlapping offset order. Exact payload size and
 * alignment are checked against the target's canonical scalar/String layout.
 * An empty layout is absent, with all fields zero. */
XR_FUNC XrLirStatus xr_lir_set_state_layout(XrLirModule *, const XrLirStateLayout *);
/* Only this definition receives the execution owner's injected STATE. It has
 * exactly one STATE parameter, trailing all ordinary parameters. Other stateful
 * definitions receive the same trailing capability through verified calls.
 * External functions and intrinsic signatures cannot contain capabilities. */
XR_FUNC XrLirStatus xr_lir_set_state_entry(XrLirModule *, XrLirFunction);
/* Only block parameters may have an explicitly supplied reference shape.
 * Finish checks every incoming edge and requires a real reference producer.
 * Different slots may merge when their payload and mutability match exactly.
 * Function parameters/results cannot contain STATE_REF. */
XR_FUNC XrLirStatus xr_lir_set_state_reference(XrLirModule *, XrLirValue,
    const XrLirStateReference *);

/* Finished-module queries borrow only the receiving module's owned storage.
 * No layout returns an empty view and no entry returns a zero handle.
 * A reference's layout identity is its XrLirValue.module. Backends preserve
 * that identity and the exact shape through moves, phis and spills. */
XR_FUNC XrLirStatus xr_lir_state_layout_view(const XrLirModule *, XrLirStateLayout *);
XR_FUNC XrLirStatus xr_lir_state_entry(const XrLirModule *, XrLirFunction *);
XR_FUNC XrLirStatus xr_lir_state_reference_view(const XrLirModule *, XrLirValue,
    XrLirStateReference *);

/* MODULE_BEGIN/READY use bits as a module ordinal. STATE_CONSTRUCT,
 * INITIALIZE and REFERENCE use bits as a slot ordinal. All other state ops
 * require zero bits. READ produces the reference payload type. WRITE requires
 * an exactly matching payload and a mutable reference. PUBLISH is mutable-only.
 * CONSTRUCT/INITIALIZE/WRITE omit the value operand for the Unit payload.
 * STATE and STATE_REF have no constant, cast, native-result or user-return path.
 * General reference parameters/call windows remain outside this interface. */
#endif // XLIR_STATE_H
