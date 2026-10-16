/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_value_internal.h - Physical view handles and independent scope admission
 *
 * KEY CONCEPT:
 *   Self-validation and physical ownership never authenticate a live origin.
 */
#ifndef XXIR_VIEW_VALUE_INTERNAL_H
#define XXIR_VIEW_VALUE_INTERNAL_H
#include "xxir_view_storage_internal.h"

/* The actual admitted caller authenticates the current binding and complete
 * origin lifetime before returning its authority. Callback success alone does
 * not bypass the storage's exact loan, range or writable checks. */
typedef XrXirValueStatus (*XrXirViewAuthorization)(void *context,
    const XrXirValue *value, bool writable, XrXirValueAdmission *admission,
    XrXirCellAuthority *authority);
typedef struct XrXirViewAccess {
    XrXirValueAdmission *values;
    XrXirViewAuthorization authorize;
    void *context;
} XrXirViewAccess;

/* A physical origin pin is not a logical handle or an access permission. Its
 * noncopyable record keeps one exact identity stable after logical retirement. */
typedef struct XrXirViewOriginPin {
    void *body;
    XrXirCellAuthority authority;
} XrXirViewOriginPin;
typedef struct XrXirViewOriginInfo {
    const XrXirValue *root;
    XrXirCellAuthority authority;
    uint32_t kind;
    XrXirType element;
    int64_t offset, length;
    bool logical_active;
} XrXirViewOriginInfo;
/* Only pin creation authenticates the current logical binding. The metadata
 * snapshot never proves a live origin; root is borrowed until pin drop. */
XR_FUNC XrXirValueStatus xr_xir_view_origin_pin(const XrXirValue *value,
    const XrXirViewAccess *access, XrXirViewOriginPin *output);
XR_FUNC XrXirValueStatus xr_xir_view_origin_info(const XrXirViewOriginPin *pin,
    XrXirValueAdmission *admission, XrXirViewOriginInfo *output);
/* Empty release is idempotent; terminal pin retirement cannot allocate or fail. */
XR_FUNC void xr_xir_view_origin_unpin(XrXirViewOriginPin *pin);

/* Only the authenticated root producer constructs a view. The caller preserves
 * the actual origin scope and any parent frame until logical retirement;
 * physical pins cannot prove that lifetime. A move transfers only the handle. */
XR_FUNC XrXirValueStatus xr_xir_view_new(XrXirType type, const XrXirValue *root,
    const XrXirCellAuthority *parent, const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_view_reborrow(XrXirType type,
    const XrXirValue *parent, const XrXirViewStorageSpec *spec,
    const XrXirViewAccess *access, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_view_len(const XrXirValue *value,
    const XrXirViewAccess *access, int64_t *output);
XR_FUNC XrXirValueStatus xr_xir_view_get(const XrXirValue *value, int64_t index,
    const XrXirViewAccess *access, XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_view_set(const XrXirValue *value, int64_t index,
    const XrXirValue *element, const XrXirViewAccess *access, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_view_copy(XrXirType array, const XrXirValue *value,
    const XrXirViewAccess *access, XrXirValue *output);
/* Local aggregate publication consumes all fields only after every admission
 * and allocation succeeds. Ordinary admission still rejects borrowed storage. */
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_admit(const XrXirValue *value,
    XrXirType type, const XrXirViewAccess *access);
/* Capture the unique declared view leaf without consuming Tuple/Optional
 * owners. None succeeds with a canonical empty pin; failures publish nothing. */
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_origin_pin(const XrXirValue *value,
    XrXirType type, const XrXirViewAccess *access, XrXirViewOriginPin *output);
/* Match every actual returned leaf to one pinned view input, including retired
 * physical parents. An empty candidate accepts only values with no view leaf.
 * Inputs remain held and unchanged for the whole check. This matches ancestry,
 * never adopts authority or proves the root lifetime. */
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_origin_match(const XrXirValue *value,
    XrXirType type, const XrXirViewOriginPin *candidate, const XrXirViewAccess *access);
XR_FUNC XrXirValueStatus xr_xir_borrowed_tuple_new_move(XrXirType type,
    XrXirValue *fields, uint32_t count, const XrXirViewAccess *access, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_borrowed_some_new_move(XrXirType type,
    XrXirValue *value, const XrXirViewAccess *access, XrXirValue *output);
/* Unpacking consumes a uniquely owned aggregate after all scope checks succeed.
 * Output buffers are caller-owned canonical empty handles. */
XR_FUNC XrXirValueStatus xr_xir_borrowed_tuple_unpack_move(XrXirValue *value,
    XrXirValue *fields, uint32_t count, const XrXirViewAccess *access);
XR_FUNC XrXirValueStatus xr_xir_borrowed_some_take(XrXirValue *value,
    const XrXirViewAccess *access, XrXirValue *output);
#endif // XXIR_VIEW_VALUE_INTERNAL_H
