/* Internal VM adapter for zero-allocation grapheme byte ranges. */

#ifndef XVM_GRAPHEME_ITERATOR_H
#define XVM_GRAPHEME_ITERATOR_H

#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue.h"
#include "../shared/xr_unicode_grapheme.h"

typedef struct XrVmGraphemeIterator {
    XrString *source;
    XrGraphemeCursor cursor;
} XrVmGraphemeIterator;

/* The adapter owns one strong reference to source until dispose. It is an
 * internal stack/value object; public Iterator/Slice provenance is deferred to
 * task 197 and task 199 P4. */
XR_FUNC bool xr_grapheme_iterator_init(XrVmGraphemeIterator *iterator, XrString *source);
XR_FUNC void xr_grapheme_iterator_dispose(XrVmGraphemeIterator *iterator);

/* Fill a caller-owned frame-local span slot. No iterator, substring, span, or
 * owner object is allocated. out_range may be NULL. */
XR_FUNC bool xr_grapheme_iterator_next(XrVmGraphemeIterator *iterator, XrSliceView *out_span,
                                       XrByteRange *out_range);

#endif /* XVM_GRAPHEME_ITERATOR_H */
