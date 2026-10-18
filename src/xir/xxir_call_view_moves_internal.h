/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_moves_internal.h - Actual frame-owned borrowed transfers
 */
#ifndef XXIR_CALL_VIEW_MOVES_INTERNAL_H
#define XXIR_CALL_VIEW_MOVES_INTERNAL_H
#include "xxir_call_view_internal.h"
#include "xxir.h"
typedef struct XrXirViewPhiTransfer {
    XrXirType type;
    uint32_t source,destination,scratch;
} XrXirViewPhiTransfer;
typedef struct XrXirViewPhiBatch {
    void *frame;
    uint32_t frame_bytes;
    const XrXirViewPhiTransfer *transfers;
    uint32_t count;
} XrXirViewPhiBatch;

typedef struct XrXirViewEndWord { XrXirType type; uint32_t offset; } XrXirViewEndWord;
typedef struct XrXirViewEndBatch {
    void *frame;
    uint32_t frame_bytes;
    const XrXirViewEndWord *words;
    uint32_t count;
} XrXirViewEndBatch;
/* Inputs and output are owned full Value slots in the actual active frame.
 * Callers copy Shared fields and stage Mut owners before entering; failures
 * leave every field and the empty output unchanged. This grants no SSA move
 * opcode, Source lifetime, root permission or ability to capture a view. */
XR_FUNC XrXirValueStatus xr_xir_call_view_aggregate(XrXirCallView *view,
    XrXirOp operation,XrXirType type,XrXirValue *fields,uint32_t count,XrXirValue *output);
/* Descriptors and word types come from an admitted backend function/layout.
 * Only real backend frame words are transferred. Metadata must be disjoint
 * from those words; scratch words are distinct, empty and outside every source
 * and destination. All authentication and fallible copies precede commit.
 * Shared sources keep their owners; Mut sources move simultaneously. */
XR_FUNC XrXirValueStatus xr_xir_call_view_phi(XrXirCallView *view,const XrXirViewPhiBatch *batch);
/* Reclaims actual backend owners after a proved instruction or PHI edge.
 * Empty words are allowed for paths on which no owner was published. Types
 * and offsets must come from the admitted function/layout, never an access
 * certificate. Validation and quota precede all clearing; a failed batch
 * preserves every word. Retired physical owners may be destroyed, never read.
 * This creates no logical END, Source lifetime or root permission proof. */
XR_FUNC XrXirValueStatus xr_xir_call_view_end_words(XrXirCallView *view,const XrXirViewEndBatch *batch);
#endif // XXIR_CALL_VIEW_MOVES_INTERNAL_H
