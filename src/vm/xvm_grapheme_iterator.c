#include "xvm_grapheme_iterator.h"

#include "../base/xchecks.h"
#include "../runtime/mem/xruntime_object_heap.h"
#include "../shared/xr_elem_type.h"

bool xr_vm_grapheme_iterator_init(XrVmGraphemeIterator *iterator, XrString *source) {
    if (!iterator || !source)
        return false;
    iterator->source = source;
    XR_CHECK(xr_runtime_object_header_retain(&source->header) ==
                 XR_RUNTIME_ABI_OK,
             "VM grapheme iterator rejected canonical string retain");
    xr_grapheme_cursor_init(&iterator->cursor, (const uint8_t *) source->data, source->length);
    return true;
}

void xr_vm_grapheme_iterator_dispose(XrVmGraphemeIterator *iterator) {
    XrString *source;
    bool last = false;
    XrRuntimeAbiStatus status;
    if (!iterator || !iterator->source)
        return;
    source = iterator->source;
    iterator->source = NULL;
    iterator->cursor.data = NULL;
    iterator->cursor.length = 0;
    iterator->cursor.offset = 0;
    iterator->cursor.state = 0;
    iterator->cursor.last_rule = XR_GRAPHEME_RULE_NONE;
    status = xr_runtime_object_header_release(&source->header, &last);
    XR_CHECK(status == XR_RUNTIME_ABI_OK,
             "VM grapheme iterator rejected canonical string release");
    if (last) {
        status = xr_runtime_object_reclaim(&source->header);
        XR_CHECK(status == XR_RUNTIME_ABI_OK,
                 "VM grapheme iterator rejected canonical string reclaim");
    }
}

bool xr_vm_grapheme_iterator_next(XrVmGraphemeIterator *iterator, XrSliceView *out_span,
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
    if (out_range)
        *out_range = range;
    return true;
}
