/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_origin_internal.h - Prepared sticky publication of actual identities
 *
 * KEY CONCEPT:
 *   Publication changes no loan, RC authority or executable permission.
 */
#ifndef XXIR_CLASS_ORIGIN_INTERNAL_H
#define XXIR_CLASS_ORIGIN_INTERNAL_H
#include "xxir_value.h"

enum {
    XIR_CLASS_ORIGIN_MODULE = 1u,
    XIR_CLASS_ORIGIN_PREPARED = 2u,
    XIR_CLASS_ORIGIN_BORROWED = 4u,
    XIR_CLASS_ORIGIN_VALID = 7u
};
/* The caller holds the replacement until drop. Retained finite nodes also
 * keep every prepared identity alive across commit and release callbacks. */
typedef struct XrXirClassPublication {
    XrXirValueAdmission *admission;
    XrXirValue *values;
    size_t count, capacity, bytes;
    bool operation, committed;
    /* Stack-scoped route identity, valid only while its actual loan stays top.
     * It is neither retained executable permission nor a publication source. */
    const void *borrow_owner, *borrow_loan;
} XrXirClassPublication;
/* Output must be zero; failure preserves output and all publication bits. */
XR_FUNC XrXirValueStatus xr_xir_class_publication_prepare(const XrXirValue *values,
    uint32_t count, XrXirValueAdmission *admission, XrXirClassPublication *output);
/* Every commit write and drop loop was prepaid by the same admission owner. */
XR_FUNC void xr_xir_class_publication_commit(XrXirClassPublication *publication);
XR_FUNC void xr_xir_class_publication_drop(XrXirClassPublication *publication);
/* This is an actual storage fact, never an execution permission. */
XR_FUNC bool xr_xir_class_module_storage(const XrXirValue *value);
#endif
