/*
 * xray_hosted_fragment_abi.h - canonical ABI for native code hosted by Xray
 *
 * A hosted fragment owns neither a runtime nor an allocator.  Every call is
 * scoped to one host/isolate context and every non-immediate value crosses the
 * boundary through these operations.  The ABI is intentionally independent
 * of XrVMRuntime so generated objects can be compiled without VM headers.
 */

#ifndef XRAY_HOSTED_FRAGMENT_ABI_H
#define XRAY_HOSTED_FRAGMENT_ABI_H

#include "xray_value_abi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_HOSTED_FRAGMENT_ABI_VERSION UINT32_C(7)

typedef enum XrHostedFragmentStatus {
    XR_HOSTED_FRAGMENT_RETURN = 0,
    XR_HOSTED_FRAGMENT_ERROR = 1,
    XR_HOSTED_FRAGMENT_SUSPEND = 2,
    XR_HOSTED_FRAGMENT_INVALID_CALL = 3,
} XrHostedFragmentStatus;

typedef enum XrHostedFragmentSuspendKind {
    XR_HOSTED_FRAGMENT_SUSPEND_BLOCKED = 1,
    XR_HOSTED_FRAGMENT_SUSPEND_YIELD = 2,
} XrHostedFragmentSuspendKind;

typedef enum XrHostedFragmentOwnership {
    XR_HOSTED_FRAGMENT_IMMEDIATE = 0,
    XR_HOSTED_FRAGMENT_BORROWED = 1,
    XR_HOSTED_FRAGMENT_OWNED = 2,
    XR_HOSTED_FRAGMENT_RETAINED = 3,
} XrHostedFragmentOwnership;

typedef struct XrHostedFragmentStringView {
    const char *data;
    size_t byte_length;
    size_t rune_length;
    uint32_t hash;
} XrHostedFragmentStringView;

/* Container payloads are never exposed as a raw VM/AOT object layout.  The
 * host reports only the canonical element-storage tag and length; individual
 * reference-bearing elements cross through value operations below. */
typedef struct XrHostedFragmentArrayView {
    uint64_t length;
    uint8_t elem_type;
    uint8_t reserved8[7];
} XrHostedFragmentArrayView;

/* A call-scoped borrow of contiguous byte storage.  Both Array<u8> and
 * Slice<u8> use this operation: generated fragments must never reinterpret
 * a host container header.  `readonly` preserves the host's dynamic view
 * provenance so a `ref` parameter can fail closed before native code runs. */
typedef struct XrHostedFragmentByteSpanView {
    uint8_t *data;
    uint64_t length;
    uint8_t readonly;
    uint8_t reserved8[7];
} XrHostedFragmentByteSpanView;

/* Class instances retain their native representation and identity inside the
 * generated fragment. The VM stores `native_value` only as an opaque owned
 * handle in a generated proxy instance; it never casts the pointer to a VM
 * object layout. object_view borrows that handle for one call. object_new
 * consumes one owning native reference on success and consumes nothing on
 * failure. */
typedef struct XrHostedFragmentObjectView {
    const char *nominal_owner;
    const char *type_name;
    XrValue native_value;
} XrHostedFragmentObjectView;

typedef enum XrHostedFragmentValueViewKind {
    XR_HOSTED_FRAGMENT_VALUE_IMMEDIATE = 0,
    XR_HOSTED_FRAGMENT_VALUE_STRING_UTF8 = 1,
} XrHostedFragmentValueViewKind;

/* A layout-neutral description for values nested inside a hosted result or
 * error.  Heap pointers are never handed to the other runtime.  More kinds
 * are added only together with an explicit ownership rule. */
typedef struct XrHostedFragmentValueView {
    uint8_t kind;
    uint8_t reserved8[7];
    XrValue immediate;
    const char *data;
    size_t byte_length;
} XrHostedFragmentValueView;

#define XR_HOSTED_FRAGMENT_MAX_ENUM_PAYLOADS UINT32_C(16)

typedef struct XrHostedFragmentEnumView {
    const char *nominal_owner;
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t layout_id;
    uint32_t payload_count;
    XrHostedFragmentValueView payloads[XR_HOSTED_FRAGMENT_MAX_ENUM_PAYLOADS];
} XrHostedFragmentEnumView;

/* The normal path returns XrValue directly.  Control information is out-of-band
 * so Win64 and other C ABIs do not introduce a second, larger hidden sret
 * buffer around XrValue.  The caller zero-initializes one signal per call;
 * generated code writes it only for non-return control. */
typedef struct XrHostedFragmentSignal {
    uint8_t status;
    uint8_t suspend_kind;
    uint16_t argument_index;
    uint32_t reserved32;
    XrValue error;
    void *continuation;
} XrHostedFragmentSignal;

typedef struct XrHostedFragmentHostOps {
    uint32_t abi_version;
    uint32_t struct_size;

    bool (*string_view)(void *host, XrValue value, XrHostedFragmentStringView *out);
    XrValue (*string_new_utf8)(void *host, const char *data, size_t byte_length, size_t rune_length,
                               uint32_t hash);
    XrValue (*error_new_utf8)(void *host, int32_t code, const char *message, size_t byte_length);
    XrValue (*enum_new)(void *host, const char *module_name, const char *enum_name,
                        const char *member_name, const XrHostedFragmentValueView *payloads,
                        uint32_t payload_count);
    bool (*enum_view)(void *host, XrValue value, XrHostedFragmentEnumView *out);

    bool (*array_view)(void *host, XrValue value, XrHostedFragmentArrayView *out);
    bool (*array_get)(void *host, XrValue value, uint64_t index, XrValue *out);
    XrValue (*array_new)(void *host, uint64_t length, uint8_t elem_type);
    bool (*array_set)(void *host, XrValue array, uint64_t index, XrValue value);
    bool (*byte_span_view)(void *host, XrValue value, XrHostedFragmentByteSpanView *out);

    bool (*object_view)(void *host, XrValue value, XrHostedFragmentObjectView *out);
    XrValue (*object_new)(void *host, const char *nominal_owner, const char *type_name,
                          XrValue native_value);

    /* Reference operations apply only to canonical header-bearing host values.
     * Immediate and borrowed values are no-ops. */
    void (*retain)(void *host, XrValue value);
    void (*release)(void *host, XrValue value);
} XrHostedFragmentHostOps;

typedef struct XrHostedFragmentContext {
    const XrHostedFragmentHostOps *ops;
    void *host;         /* one isolate/call host, never process-global state */
    void *module_state; /* generated per-isolate module state */
    void *coroutine;    /* host-owned current coroutine, NULL for sync calls */
    /* Opaque canonical XrAotVmHostOps table borrowed from the host.  Generated
     * code never inspects this table directly; it installs it in a scoped
     * XrAotContext while the fragment runs. */
    const void *runtime_ops;
    /* Non-NULL only when the host resumes a previously suspended entry.  The
     * pointer was published by XrHostedFragmentSignal.continuation and remains
     * owned by the generated entry until it returns or reports an error. */
    void *continuation;
    const char *module_name;        /* nominal owner used for type identity imports */
    XrHostedFragmentSignal *signal; /* caller-owned, zeroed before every call */
} XrHostedFragmentContext;

typedef XrValue (*XrHostedFragmentEntry)(const XrHostedFragmentContext *context,
                                         const XrValue *arguments, uint32_t argument_count);

static inline XrValue xr_hosted_fragment_invalid_call(const XrHostedFragmentContext *context,
                                                      uint16_t argument_index) {
    if (context && context->signal) {
        context->signal->status = XR_HOSTED_FRAGMENT_INVALID_CALL;
        context->signal->argument_index = argument_index;
    }
    return (XrValue) {0};
}

#if defined(__cplusplus)
static_assert(offsetof(XrHostedFragmentSignal, error) == 8, "hosted fragment signal ABI changed");
#else
_Static_assert(offsetof(XrHostedFragmentSignal, error) == 8, "hosted fragment signal ABI changed");
#endif

#endif /* XRAY_HOSTED_FRAGMENT_ABI_H */
