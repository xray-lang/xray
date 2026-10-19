/*
 * xray_value_abi.h - canonical hosted XrValue binary layout
 *
 * This header is the single definition consumed by the VM, hosted AOT, the
 * public runtime API, and generated stdlib-native entries.  Tag namespaces may
 * add backend-private values, but they may not redefine this carrier layout.
 */

#ifndef XRAY_VALUE_ABI_H
#define XRAY_VALUE_ABI_H

#include <stddef.h>
#include <stdint.h>

#define XR_HOSTED_OBJECT_ABI_VERSION UINT32_C(1)

/* The pointer payload is the address of the canonical XrObjHeader itself.
 * This bit is required when a value has crossed a type-erased owner boundary
 * and heap_type can no longer identify the concrete AOT allocation kind. */
#define XR_VALUE_FLAG_HEADER_AT_PTR 0x02u

typedef struct XrValue {
    union {
        struct {
            uint8_t tag;        /* [0]   backend tag */
            uint8_t flags;      /* [1]   ownership/layout flags */
            uint16_t heap_type; /* [2-3] object subtype for pointer values */
            uint32_t ext;       /* [4-7] reserved, zero unless specified */
        };
        uint64_t descriptor; /* [0-7] bulk metadata load/compare */
    };
    union {
        int64_t i; /* [8-15] integer payload */
        double f;  /* [8-15] floating-point payload */
        void *ptr; /* [8-15] pointer payload */
    };
} XrValue;

#define XRVAL_OFF_TAG 0
#define XRVAL_OFF_FLAGS 1
#define XRVAL_OFF_HEAP_TYPE 2
#define XRVAL_OFF_EXT 4
#define XRVAL_OFF_PAYLOAD 8
#define XRVAL_SIZE 16

#if defined(__cplusplus)
static_assert(sizeof(XrValue) == XRVAL_SIZE, "XrValue ABI size changed");
static_assert(offsetof(XrValue, tag) == XRVAL_OFF_TAG, "XrValue.tag ABI changed");
static_assert(offsetof(XrValue, flags) == XRVAL_OFF_FLAGS, "XrValue.flags ABI changed");
static_assert(offsetof(XrValue, heap_type) == XRVAL_OFF_HEAP_TYPE,
              "XrValue.heap_type ABI changed");
static_assert(offsetof(XrValue, ext) == XRVAL_OFF_EXT, "XrValue.ext ABI changed");
static_assert(offsetof(XrValue, i) == XRVAL_OFF_PAYLOAD, "XrValue payload ABI changed");
#else
_Static_assert(sizeof(XrValue) == XRVAL_SIZE, "XrValue ABI size changed");
_Static_assert(offsetof(XrValue, tag) == XRVAL_OFF_TAG, "XrValue.tag ABI changed");
_Static_assert(offsetof(XrValue, flags) == XRVAL_OFF_FLAGS, "XrValue.flags ABI changed");
_Static_assert(offsetof(XrValue, heap_type) == XRVAL_OFF_HEAP_TYPE,
               "XrValue.heap_type ABI changed");
_Static_assert(offsetof(XrValue, ext) == XRVAL_OFF_EXT, "XrValue.ext ABI changed");
_Static_assert(offsetof(XrValue, i) == XRVAL_OFF_PAYLOAD, "XrValue payload ABI changed");
#endif

#define XR_VALUE_DEFINED 1

#endif /* XRAY_VALUE_ABI_H */
