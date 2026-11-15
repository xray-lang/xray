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
struct XrRbcCodeOwner {
    _Atomic(uint32_t) references;
    const XrRbcFunction *functions;
    size_t function_count;
    XrLirValueHomes *const *value_homes;
    XrRbcValueCode *const *value_codes;
    const XrtValueImage *values;
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
#endif // XRBC_CODE_OWNER_H
