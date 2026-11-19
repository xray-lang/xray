/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_values.h - Execution-owned fixed-value context retirement
 */
#ifndef XNATIVE_VALUES_H
#define XNATIVE_VALUES_H
#include "xnative_value.h"
#include "xnative_managed.h"
#include "xnative_backing_types.h"
#include "xray_native_state.h"

/* Cold execution setup can precede the active source-map/recovery binding.
 * Success lends the one actual billing owner while this outer root is live;
 * no managed frame, backing setup or guest value is created. Failure leaves
 * *empty unchanged. The caller drains this root if later setup fails. */
XR_FUNC XrtManagedStatus xrt_native_values_prepare(XrtValueRoot *, XrtBillingOwner **empty);
/* Borrow the one cold setup owned by this execution context. All values keep
 * their own Ops leases; the context remains the caller's actual outer owner.
 * Failure preserves output and no frame or guest value is created here. */
XR_FUNC XrtManagedStatus xrt_native_values_backings(XrtValueRoot *, const XrtBackingTypes **empty);
/* The native execution root has already bound this image to its State.
 * This synchronous association is shared by every helper in that root. */
XR_FUNC void xrt_native_values_use_state(XrtValueRoot *, const XrtModuleState *, const XrtValueImage *);
#endif // XNATIVE_VALUES_H
