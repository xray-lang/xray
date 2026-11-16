/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_capture_backing.h - Escaping environments and necessary identity Cells
 *
 * KEY CONCEPT:
 *   Cold typed setup binds one sealed physical shape. Only escaping captures
 *   allocate; frame environments and ordinary local variables never enter it.
 */
#ifndef XNATIVE_CAPTURE_BACKING_H
#define XNATIVE_CAPTURE_BACKING_H
#include "xnative_value_plan.h"
#include "xnative_managed_root.h"
#include "xnative_map_backing.h"

typedef struct XrtCaptureType XrtCaptureType;
typedef struct XrtI64Cell {
    XrtManagedHeader managed;
    int64_t value;
} XrtI64Cell;
typedef struct XrtMapCell {
    XrtManagedHeader managed;
    XrtManagedHeader *value;
} XrtMapCell;
_Static_assert(sizeof(XrtI64Cell) == 24 && sizeof(XrtMapCell) == 24 &&
    offsetof(XrtI64Cell, value) == 16 && offsetof(XrtMapCell, value) == 16,
    "identity Cells have a sixteen-byte header and one typed payload field");

/* Move-only cold setup accepts only ENVIRONMENT or CELL<I64/Map<String,I64>>.
 * It selects and binds the physical TypePlan/Ops itself, copies the shifted
 * owning offsets and gives the TypePlan a real ValuePlan lease. A ValuePlan
 * never owns this setup or its Ops. No arbitrary Ops may be injected. */
XR_FUNC XrtManagedStatus xrt_capture_type_new(XrtBillingOwner *, const XrtValuePlan *, XrtCaptureType **empty);
XR_FUNC void xrt_capture_type_drop(XrtCaptureType **owned);

/* The complete fixed capture payload starts immediately after Header16.
 * Captures are admitted values with real published owners, borrowed for this
 * call. Prepare installs every owning slot as true NULL before the leaf copy.
 * Failure aborts only this call's successfully prepared token. Frame/stack
 * environments do not use this factory. Entry ordinals grant no code address
 * or invocation permission; that remains with the admitted code binding. */
XR_FUNC XrtManagedStatus xrt_environment_create(XrtManagedFrame *, size_t,
    const XrtCaptureType *, const void *captures);
typedef struct XrtEnvironmentBorrow {
    const XrtManagedHeader *owner;
    const XrtValuePlan *plan;
    const void *payload;
} XrtEnvironmentBorrow;
/* Establish the pair only while type is live, matching its exact producer Ops.
 * Afterwards the real owner keeps this same Plan and payload alive through its
 * own Ops, even if setup/creator/code-image callers retire. The caller holds a
 * real owner for the whole borrow. A retired type cannot recreate a view from
 * a bare Header; this view is not an additional reference or mutation grant. */
XR_FUNC XrtManagedStatus xrt_environment_borrow(const XrtCaptureType *,
    const XrtManagedHeader *, XrtEnvironmentBorrow *empty);
/* Recover a paired view after setup retirement from a real owner produced by
 * this environment factory. Its private lease and complete fixed geometry must
 * match the immutable ENVIRONMENT plan kept alive by that owner. The caller
 * holds the owner throughout the borrow; no allocation or retain occurs. This
 * proves storage provenance only: the invocation boundary still checks its
 * admitted code identity, entry ordinal and signature independently. */
XR_FUNC XrtManagedStatus xrt_environment_borrow_owned(const XrtManagedHeader *, XrtEnvironmentBorrow *empty);
XR_FUNC XrtManagedStatus xrt_environment_capture_borrow(const XrtEnvironmentBorrow *, size_t,
    const XrtValuePlan **empty_type, const void **empty_payload);

XR_FUNC XrtManagedStatus xrt_i64_cell_create(XrtManagedFrame *, size_t,
    const XrtCaptureType *, int64_t initial);
XR_FUNC XrtManagedStatus xrt_map_cell_create(XrtManagedFrame *, size_t,
    const XrtCaptureType *, const XrtMapBacking *initial);
/* These handles already have their exact admitted Cell kind and a live real
 * owner. Every operation pins that Cell and acquires its actual Header guard.
 * Read results are scalar copies or synchronous Map borrows. A Map borrow
 * requires the caller to preserve the Cell owner and exclude replacement;
 * use read_copy before callbacks or later mutations can retire that child. read_copy
 * first retains the Map and then publishes it into the stable destination. */
XR_FUNC XrtManagedStatus xrt_i64_cell_read(const XrtI64Cell *, int64_t *);
XR_FUNC XrtManagedStatus xrt_map_cell_borrow(const XrtMapCell *, const XrtMapBacking **empty);
XR_FUNC XrtManagedStatus xrt_map_cell_read_copy(XrtManagedFrame *, size_t, const XrtMapCell *);
/* The caller already owns exclusive access to the actual Cell identity.
 * References/borrows and the reentry guard do not grant write authority.
 * Cells are not COW values: multiple positive-band owners observe one identity.
 * Shared-band Cells remain read-only. Map writes retain before publication and
 * release the previous child afterwards, including pointer-equal writes. */
XR_FUNC XrtManagedStatus xrt_i64_cell_write_exclusive(XrtI64Cell *, int64_t);
XR_FUNC XrtManagedStatus xrt_map_cell_write_exclusive(XrtMapCell *, const XrtMapBacking *);
/* A borrowed descriptor of the actual Map child and Cell guard. The caller
 * keeps a real Cell/Env owner for its complete lifetime. MOVE_FROM is rejected:
 * a complete Map Cell never loses its non-NULL child to a consuming read. */
XR_FUNC XrtManagedStatus xrt_map_cell_external(XrtMapCell *, XrtExternalOwner *empty);
XR_FUNC XrtManagedStatus xrt_map_cell_update_exclusive(XrtMapCell *, XrtManagedFrame *,
    size_t workspace, const XrtBackingTypes *, const XrtMapUpdate *, bool *removed);
#endif // XNATIVE_CAPTURE_BACKING_H
