/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_callable.h - Owned canonical closures over authenticated code leases
 */
#ifndef XNATIVE_CALLABLE_H
#define XNATIVE_CALLABLE_H
#include "xir/xxir_type_arena.h"
#include "xray_native_callable.h"

typedef struct XrtCallableCode XrtCallableCode;

XR_FUNC bool xrt_callable_type_id(XrtCallableValueSpec, uint32_t count, XrXirType *);
/* One physical pool is projected into an independently owned canonical arena.
 * Throwing signatures remain unsupported until the complete error ABI exists.
 * The limit covers the temporary receiving workspace and retained type arena. */
XR_FUNC XrXirValueStatus xrt_callable_types_new(const XrtCallableTypeSpec *, uint32_t count,
    uint64_t limit, XrXirTypeArena **empty);

typedef struct XrtCallableEntry {
    XrXirType type;
    const XrXirType *captures;
    uint32_t capture_count;
} XrtCallableEntry;

/* The receiving code owner authenticates these entry ordinals. Descriptor
 * shape never supplies an executable address or a module-state owner. */
typedef struct XrtCallableLease {
    void *owner;
    /* Atomic metadata retain, with false denoting its reference limit. */
    bool (*retain)(void *);
    void (*release)(void *);
} XrtCallableLease;

/* Entries and capture vectors are copied. The arena and executable lease are
 * retained before publication. Failure preserves an empty output. */
XR_FUNC XrXirValueStatus xrt_callable_code_new(XrXirTypeArena *, const XrtCallableEntry *,
    uint32_t count, XrtCallableLease, XrtCallableCode **empty);
XR_FUNC bool xrt_callable_code_retain(XrtCallableCode *);
XR_FUNC void xrt_callable_code_drop(XrtCallableCode **);

/* Success creates a real canonical Fn and takes its own code lease. Captures
 * keep their actual canonical identities. No activation pointer is accepted. */
XR_FUNC XrXirValueStatus xrt_callable_new(XrtCallableCode *, XrXirDomain *, uint32_t entry,
    const XrXirValue *captures, uint32_t count, XrXirValue *empty);
/* Exact canonical Cell producers use the same arena and graph Domain as their
 * closures. The caller closes a cycle-capable Domain at its actual owner end. */
XR_FUNC XrXirValueStatus xrt_callable_cell_new(XrtCallableCode *, XrXirDomain *, XrXirType,
    const XrXirValue *initial, XrXirValue *empty);
XR_FUNC XrXirValueStatus xrt_callable_cell_write(XrtCallableCode *, XrXirDomain *, XrXirType,
    const XrXirValue *cell, const XrXirValue *replacement);
/* The caller retains code and value while using this synchronous borrow.
 * The returned capture vector ends with the actual canonical Fn owner. */
XR_FUNC XrXirValueStatus xrt_callable_borrow(const XrtCallableCode *, const XrXirValue *,
    uint32_t entry, const XrXirValue **captures, uint32_t *count);
struct XrtValueRoot;
XR_FUNC void xrt_callable_root_drain(struct XrtValueRoot *);
#endif // XNATIVE_CALLABLE_H
