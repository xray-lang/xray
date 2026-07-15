#include "xvm_grapheme_iterator.h"

#include "../base/xchecks.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../shared/xr_elem_type.h"

bool xr_vm_grapheme_iterator_init(XrVmGraphemeIterator *iterator, XrString *source) {
    if (!iterator || !source)
        return false;
    iterator->source = source;
    xr_rc_retain((XrObjHeader *) source);
    xr_grapheme_cursor_init(&iterator->cursor, (const uint8_t *) source->data, source->length);
    return true;
}

void xr_vm_grapheme_iterator_dispose(XrVmGraphemeIterator *iterator, XrCoroHeap *heap) {
    XrString *source;
    if (!iterator || !iterator->source)
        return;
    source = iterator->source;
    iterator->source = NULL;
    iterator->cursor.data = NULL;
    iterator->cursor.length = 0;
    iterator->cursor.offset = 0;
    iterator->cursor.state = 0;
    iterator->cursor.last_rule = XR_GRAPHEME_RULE_NONE;
    xr_rc_release(heap, (XrObjHeader *) source);
}

bool xr_vm_grapheme_iterator_next(XrVmGraphemeIterator *iterator, XrSpanView *out_span,
                                  XrByteRange *out_range) {
    XrByteRange range;
    XrString *source;
    XR_DCHECK(iterator != NULL, "VM grapheme iterator must not be NULL");
    XR_DCHECK(out_span != NULL, "VM grapheme iterator span slot must not be NULL");
    if (!iterator || !out_span || !(source = iterator->source))
        return false;
    if (!xr_grapheme_cursor_next(&iterator->cursor, &range))
        return false;

    out_span->data = (void *) ((uint8_t *) source->data + range.start);
    out_span->length = (int64_t) (range.end - range.start);
    out_span->elem_type = XR_ELEM_U8;
    out_span->elem_size = 1;
    out_span->elem_tid = 0;
    out_span->contains_refs = 0;
    out_span->reserved = XR_SPAN_VIEW_READONLY;
    /* The iterator, not the Slice value, owns the source root. */
    out_span->guard = NULL;
    if (out_range)
        *out_range = range;
    return true;
}
