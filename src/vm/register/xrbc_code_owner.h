/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_code_owner.h - Actual executable ownership without compiler linkage
 *
 * KEY CONCEPT:
 *   The producer embeds this owner in its one immutable code allocation. Queries
 *   borrow its real assets; no registry, runtime Layout or validation flag exists.
 */
#ifndef XRBC_CODE_OWNER_H
#define XRBC_CODE_OWNER_H
#include "xrbc_value_code.h"
#include <stdatomic.h>

typedef struct XrRbcCodeOwner XrRbcCodeOwner;
typedef enum XrRbcCallableEntryKind {
    XR_RBC_CALLABLE_ENTRY_CODE, XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION
} XrRbcCallableEntryKind;
typedef struct XrRbcCallableEntry {
    const XrRbcCodeOwner *producer; /* Borrowed enclosing code allocation. */
    uint32_t physical_function, source_function, signature_type, environment_type;
    XrRbcCallableEntryKind kind;
    uint32_t callable;
} XrRbcCallableEntry;
struct XrRbcCodeOwner {
    _Atomic(uint32_t) references;
    const XrRbcFunction *functions;
    size_t function_count;
    uint32_t startup_function; /* One-based physical execution root. */
    XrLirValueHomes *const *value_homes;
    XrRbcValueCode *const *value_codes;
    const XrtValueImage *values;
    const XrRbcModule *module;
    XrtLocationOwner *locations;
    /* Ordinary entries form the physical prefix; intrinsic entries have no code body. */
    const XrRbcCallableEntry *entries;
    size_t entry_count;
    const uint32_t *type_callables; /* Image type -> one-based RBC signature, zero for non-Fn. */
    /* The enclosing allocation owns every borrowed asset above. Retire runs
     * exactly once at zero actual references and must return normally. The
     * producer keeps its callback code live through this final retirement. */
    void (*retire)(XrRbcCodeOwner *);
};

/* Only the receiving producer initializes this owner, before publishing any
 * lease. Fields other than the real reference count are immutable thereafter.
 * These operations neither infer nor authorize a new owning or borrowing fact. */
XR_FUNC bool xr_rbc_code_owner_retain(void *);
XR_FUNC void xr_rbc_code_owner_release(void *);
/* A genuine producer lease protects its own raw snapshot, never a Program
 * shell or the caller's current module. The returned owner is only borrowed. */
XR_FUNC const XrRbcCodeOwner *xr_rbc_code_owner_borrow(XrtManagedLease);
XR_FUNC const XrRbcCodeOwner *xr_rbc_module_code_owner(const XrRbcModule *);
XR_FUNC const XrRbcCallableEntry *xr_rbc_module_callable_entries(const XrRbcModule *);
XR_FUNC size_t xr_rbc_module_callable_entry_count(const XrRbcModule *);
XR_FUNC const uint32_t *xr_rbc_module_type_callables(const XrRbcModule *);
#endif // XRBC_CODE_OWNER_H
