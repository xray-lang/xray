/* Unicode 17.0.0 generated grapheme property lookup shared by VM and AOT. */

#ifndef XR_UNICODE_GRAPHEME_DATA_H
#define XR_UNICODE_GRAPHEME_DATA_H

#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XrGraphemeBreakClass {
    XR_GCB_OTHER = 0,
    XR_GCB_CR = 1,
    XR_GCB_LF = 2,
    XR_GCB_CONTROL = 3,
    XR_GCB_EXTEND = 4,
    XR_GCB_ZWJ = 5,
    XR_GCB_REGIONAL_INDICATOR = 6,
    XR_GCB_PREPEND = 7,
    XR_GCB_SPACING_MARK = 8,
    XR_GCB_L = 9,
    XR_GCB_V = 10,
    XR_GCB_T = 11,
    XR_GCB_LV = 12,
    XR_GCB_LVT = 13,
} XrGraphemeBreakClass;

typedef enum XrIndicConjunctBreak {
    XR_INCB_NONE = 0,
    XR_INCB_CONSONANT = 1,
    XR_INCB_EXTEND = 2,
    XR_INCB_LINKER = 3,
} XrIndicConjunctBreak;

#define XR_GRAPHEME_PROPERTY_GCB_MASK UINT8_C(0x0f)
#define XR_GRAPHEME_PROPERTY_EXTENDED_PICTOGRAPHIC UINT8_C(0x10)
#define XR_GRAPHEME_PROPERTY_INCB_SHIFT 5
#define XR_GRAPHEME_PROPERTY_INCB_MASK UINT8_C(0x60)

XR_FUNC uint8_t xr_unicode_grapheme_property_word(uint32_t code_point);

static inline XrGraphemeBreakClass xr_grapheme_property_gcb(uint8_t property_word) {
    return (XrGraphemeBreakClass) (property_word & XR_GRAPHEME_PROPERTY_GCB_MASK);
}

static inline bool xr_grapheme_property_is_extended_pictographic(uint8_t property_word) {
    return (property_word & XR_GRAPHEME_PROPERTY_EXTENDED_PICTOGRAPHIC) != 0;
}

static inline XrIndicConjunctBreak xr_grapheme_property_incb(uint8_t property_word) {
    return (XrIndicConjunctBreak) ((property_word & XR_GRAPHEME_PROPERTY_INCB_MASK) >>
                                   XR_GRAPHEME_PROPERTY_INCB_SHIFT);
}

#endif /* XR_UNICODE_GRAPHEME_DATA_H */
