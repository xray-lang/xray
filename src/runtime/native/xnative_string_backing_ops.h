/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string_backing_ops.h - Typed operations on admitted UTF-8 storage
 *
 * KEY CONCEPT:
 *   Borrows read the fixed layout; derived owners publish through stable slots.
 */
#ifndef XNATIVE_STRING_BACKING_OPS_H
#define XNATIVE_STRING_BACKING_OPS_H
#include "xnative_string_backing.h"
#include "xray_native_place.h"
#include "xray_native_pod.h"

typedef enum XrtStringTest {
    XRT_STRING_CONTAINS, XRT_STRING_STARTS_WITH, XRT_STRING_ENDS_WITH
} XrtStringTest;
typedef struct XrtStringReplacement {
    const XrtStringBacking *search, *replacement;
    bool all;
} XrtStringReplacement;
/* Language errors are distinct from allocator/ownership failures. EmptyPattern
 * produces no Array and leaves both destination and workspace unchanged. */
typedef struct XrtStringSplitResult {
    XrtManagedStatus status;
    bool empty_pattern;
} XrtStringSplitResult;

/* Inputs are admitted live borrows, never arbitrary handles. Scalar outputs
 * stay unchanged on failure. Byte spans include embedded NULs, excluding the
 * final storage terminator; cached counts do not scan or allocate. */
XR_FUNC XrtManagedStatus xrt_string_backing_counts(const XrtStringBacking *, size_t *bytes, size_t *runes);
XR_FUNC XrtManagedStatus xrt_string_backing_compare(const XrtStringBacking *, const XrtStringBacking *, int *order);
XR_FUNC XrtManagedStatus xrt_string_backing_test(const XrtStringBacking *, const XrtStringBacking *, XrtStringTest, bool *);
XR_FUNC XrtManagedStatus xrt_string_backing_index_of(const XrtStringBacking *, const XrtStringBacking *, int64_t start, int64_t *);
XR_FUNC XrtManagedStatus xrt_string_backing_last_index_of(const XrtStringBacking *, const XrtStringBacking *, int64_t *);
XR_FUNC XrtManagedStatus xrt_string_backing_rune_at(const XrtStringBacking *, int64_t, uint32_t *);
/* Start at byte_cursor=0 and reuse each returned cursor. End returns has=false
 * without changing rune. The owning String must outlive this borrowed cursor. */
XR_FUNC XrtManagedStatus xrt_string_backing_rune_next(const XrtStringBacking *, size_t *byte_cursor, uint32_t *rune, bool *has);

/* Copy acquires one real reference before replacing even an occupied slot.
 * Constructors keep old committed inputs alive until complete publication,
 * including when an input is borrowed from the destination itself. */
XR_FUNC XrtManagedStatus xrt_string_backing_copy_into(XrtManagedFrame *, size_t, const XrtStringBacking *);
XR_FUNC XrtManagedStatus xrt_string_backing_slice_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtStringBacking *, int64_t start, int64_t end);
XR_FUNC XrtManagedStatus xrt_string_backing_repeat_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtStringBacking *, int64_t count);
XR_FUNC XrtManagedStatus xrt_string_backing_replace_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtStringBacking *, XrtStringReplacement);
XR_FUNC XrtManagedStatus xrt_string_backing_copy_bytes_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtStringBacking *);
/* Workspace is a distinct, exclusively reserved empty slot in the same Frame.
 * Each new field is rooted there before its real owner moves into the pending
 * Array. Success and operational failure both leave the workspace empty. A
 * pre-existing workspace pending token is rejected without aborting it. */
XR_FUNC XrtStringSplitResult xrt_string_backing_split_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtStringBacking *, const XrtStringBacking *, size_t workspace);

/* The caller already holds exclusive binding/storage mutation authority and
 * excludes every observing borrow. This function does not grant that authority
 * from a Root slot, RC count or pointer identity. Shared/capacity misses return
 * OK with appended=false and no changes; the caller may then concat_create.
 * Capacity is immutable. Self-append is supported under the same authority. */
XR_FUNC XrtManagedStatus xrt_string_backing_try_append_exclusive(XrtStringBacking *, const XrtStringBacking *, bool *appended);
/* The caller supplies an admitted MOVE and live RHS borrow. Geometry and all
 * three real slots are checked before an allocation-free edit. A unique source
 * with a distinct suffix may resize under the same reservation. COW prepares a
 * geometrically grown candidate; any rejection
 * preserves source/result and clears only this operation's pending candidate. */
XR_FUNC XrtManagedStatus xrt_string_backing_append_owned(XrtManagedFrame *,
    const XrtBackingTypes *, const XrtStringAppendOwned *);
/* The admitted binding remains live and is never consumed. Its distinct
 * candidate workspace is a true-empty slot, with no pending token. No source
 * retain forces COW; a different suffix is pinned before candidate allocation,
 * and the binding's real owner protects self-append. Publication and retirement
 * finish under the same Place domains. The caller supplies mutation authority. */
XR_FUNC XrtManagedStatus xrt_string_backing_append_place(XrtPlace, XrtManagedFrame *, size_t workspace,
    const XrtBackingTypes *, const XrtStringBacking *suffix);
XR_FUNC XrtManagedStatus xrt_string_backing_i64_create(XrtManagedFrame *, size_t, const XrtBackingTypes *, int64_t);
XR_FUNC XrtManagedStatus xrt_string_backing_u64_create(XrtManagedFrame *, size_t, const XrtBackingTypes *, uint64_t);
XR_FUNC XrtManagedStatus xrt_string_backing_f64_create(XrtManagedFrame *, size_t, const XrtBackingTypes *, uint64_t bits);
XR_FUNC XrtManagedStatus xrt_string_backing_bool_create(XrtManagedFrame *, size_t, const XrtBackingTypes *, bool);
XR_FUNC XrtManagedStatus xrt_string_backing_rune_create(XrtManagedFrame *, size_t, const XrtBackingTypes *, uint32_t);
/* Exact POD bytes are borrowed through the shared primitive/Nullable layout.
 * Input borrows need no type-pool lookup, retain or floating-point operation.
 * Complete text occupies one real pending String before atomic publication. */
XR_FUNC XrtManagedStatus xrt_string_backing_pod_display_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtPodDisplay *, const void *bytes);
#endif // XNATIVE_STRING_BACKING_OPS_H
