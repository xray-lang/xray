/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xr_unicode_grapheme.h"
#include "../base/xchecks.h"
#include "xr_utf8_core.h"

#define XR_GRAPHEME_STATE_PREVIOUS_MASK UINT32_C(0xff)
#define XR_GRAPHEME_STATE_RI_ODD (UINT32_C(1) << 8)
#define XR_GRAPHEME_STATE_GB11_SHIFT 9
#define XR_GRAPHEME_STATE_GB11_MASK (UINT32_C(3) << XR_GRAPHEME_STATE_GB11_SHIFT)
#define XR_GRAPHEME_STATE_INCB_SHIFT 11
#define XR_GRAPHEME_STATE_INCB_MASK (UINT32_C(3) << XR_GRAPHEME_STATE_INCB_SHIFT)

typedef enum XrGraphemeGb11State {
    XR_GB11_NONE = 0,
    XR_GB11_AFTER_EP = 1,
    XR_GB11_ZWJ_READY = 2,
} XrGraphemeGb11State;

typedef enum XrGraphemeIncbState {
    XR_INCB_STATE_NONE = 0,
    XR_INCB_STATE_AFTER_CONSONANT = 1,
    XR_INCB_STATE_HAVE_LINKER = 2,
} XrGraphemeIncbState;

static bool gcb_is_control(XrGraphemeBreakClass gcb) {
    return gcb == XR_GCB_CONTROL || gcb == XR_GCB_CR || gcb == XR_GCB_LF;
}

static uint32_t state_init(uint8_t property) {
    uint32_t state = property;
    if (xr_grapheme_property_gcb(property) == XR_GCB_REGIONAL_INDICATOR)
        state |= XR_GRAPHEME_STATE_RI_ODD;
    if (xr_grapheme_property_is_extended_pictographic(property))
        state |= (uint32_t) XR_GB11_AFTER_EP << XR_GRAPHEME_STATE_GB11_SHIFT;
    if (xr_grapheme_property_incb(property) == XR_INCB_CONSONANT)
        state |= (uint32_t) XR_INCB_STATE_AFTER_CONSONANT << XR_GRAPHEME_STATE_INCB_SHIFT;
    return state;
}

static uint32_t state_update(uint32_t state, uint8_t current) {
    uint8_t previous = (uint8_t) (state & XR_GRAPHEME_STATE_PREVIOUS_MASK);
    XrGraphemeBreakClass previous_gcb = xr_grapheme_property_gcb(previous);
    XrGraphemeBreakClass current_gcb = xr_grapheme_property_gcb(current);
    XrGraphemeGb11State previous_gb11 =
        (XrGraphemeGb11State) ((state & XR_GRAPHEME_STATE_GB11_MASK) >>
                               XR_GRAPHEME_STATE_GB11_SHIFT);
    XrGraphemeIncbState previous_incb =
        (XrGraphemeIncbState) ((state & XR_GRAPHEME_STATE_INCB_MASK) >>
                               XR_GRAPHEME_STATE_INCB_SHIFT);
    XrGraphemeGb11State next_gb11 = XR_GB11_NONE;
    XrGraphemeIncbState next_incb = XR_INCB_STATE_NONE;
    bool ri_odd = false;

    if (current_gcb == XR_GCB_REGIONAL_INDICATOR) {
        ri_odd = previous_gcb == XR_GCB_REGIONAL_INDICATOR ? (state & XR_GRAPHEME_STATE_RI_ODD) == 0
                                                           : true;
    }

    if (xr_grapheme_property_is_extended_pictographic(current)) {
        next_gb11 = XR_GB11_AFTER_EP;
    } else if (current_gcb == XR_GCB_EXTEND && previous_gb11 == XR_GB11_AFTER_EP) {
        next_gb11 = XR_GB11_AFTER_EP;
    } else if (current_gcb == XR_GCB_ZWJ && previous_gb11 == XR_GB11_AFTER_EP) {
        next_gb11 = XR_GB11_ZWJ_READY;
    }

    switch (xr_grapheme_property_incb(current)) {
        case XR_INCB_CONSONANT:
            next_incb = XR_INCB_STATE_AFTER_CONSONANT;
            break;
        case XR_INCB_EXTEND:
            next_incb = previous_incb;
            break;
        case XR_INCB_LINKER:
            if (previous_incb == XR_INCB_STATE_AFTER_CONSONANT ||
                previous_incb == XR_INCB_STATE_HAVE_LINKER)
                next_incb = XR_INCB_STATE_HAVE_LINKER;
            break;
        case XR_INCB_NONE:
            break;
    }

    state = current;
    if (ri_odd)
        state |= XR_GRAPHEME_STATE_RI_ODD;
    state |= (uint32_t) next_gb11 << XR_GRAPHEME_STATE_GB11_SHIFT;
    state |= (uint32_t) next_incb << XR_GRAPHEME_STATE_INCB_SHIFT;
    return state;
}

static bool boundary_decision(uint32_t state, uint8_t current, XrGraphemeRule *rule) {
    uint8_t previous = (uint8_t) (state & XR_GRAPHEME_STATE_PREVIOUS_MASK);
    XrGraphemeBreakClass left = xr_grapheme_property_gcb(previous);
    XrGraphemeBreakClass right = xr_grapheme_property_gcb(current);
    XrGraphemeGb11State gb11 = (XrGraphemeGb11State) ((state & XR_GRAPHEME_STATE_GB11_MASK) >>
                                                      XR_GRAPHEME_STATE_GB11_SHIFT);
    XrGraphemeIncbState incb = (XrGraphemeIncbState) ((state & XR_GRAPHEME_STATE_INCB_MASK) >>
                                                      XR_GRAPHEME_STATE_INCB_SHIFT);

    if (left == XR_GCB_CR && right == XR_GCB_LF) {
        *rule = XR_GRAPHEME_RULE_GB3;
        return false;
    }
    if (gcb_is_control(left)) {
        *rule = XR_GRAPHEME_RULE_GB4;
        return true;
    }
    if (gcb_is_control(right)) {
        *rule = XR_GRAPHEME_RULE_GB5;
        return true;
    }
    if (left == XR_GCB_L &&
        (right == XR_GCB_L || right == XR_GCB_V || right == XR_GCB_LV || right == XR_GCB_LVT)) {
        *rule = XR_GRAPHEME_RULE_GB6;
        return false;
    }
    if ((left == XR_GCB_LV || left == XR_GCB_V) && (right == XR_GCB_V || right == XR_GCB_T)) {
        *rule = XR_GRAPHEME_RULE_GB7;
        return false;
    }
    if ((left == XR_GCB_LVT || left == XR_GCB_T) && right == XR_GCB_T) {
        *rule = XR_GRAPHEME_RULE_GB8;
        return false;
    }
    if (right == XR_GCB_EXTEND || right == XR_GCB_ZWJ) {
        *rule = XR_GRAPHEME_RULE_GB9;
        return false;
    }
    if (right == XR_GCB_SPACING_MARK) {
        *rule = XR_GRAPHEME_RULE_GB9A;
        return false;
    }
    if (left == XR_GCB_PREPEND) {
        *rule = XR_GRAPHEME_RULE_GB9B;
        return false;
    }
    if (xr_grapheme_property_incb(current) == XR_INCB_CONSONANT &&
        incb == XR_INCB_STATE_HAVE_LINKER) {
        *rule = XR_GRAPHEME_RULE_GB9C;
        return false;
    }
    if (xr_grapheme_property_is_extended_pictographic(current) && gb11 == XR_GB11_ZWJ_READY) {
        *rule = XR_GRAPHEME_RULE_GB11;
        return false;
    }
    if (left == XR_GCB_REGIONAL_INDICATOR && right == XR_GCB_REGIONAL_INDICATOR &&
        (state & XR_GRAPHEME_STATE_RI_ODD) != 0) {
        *rule = XR_GRAPHEME_RULE_GB12_13;
        return false;
    }
    *rule = XR_GRAPHEME_RULE_GB999;
    return true;
}

static void emit_trace(XrGraphemeTraceFn trace, void *context, size_t offset, bool is_break,
                       XrGraphemeRule rule) {
    if (trace)
        trace(offset, is_break, rule, context);
}

static XrUtf8Step decode_valid_scalar(const uint8_t *data, size_t length) {
    XrUtf8Step step = xr_utf8_core_decode_step(data, length);
    XR_DCHECK(step.error == XR_UTF8_OK && step.consumed > 0,
              "valid UTF-8 grapheme input must decode");
    return step;
}

void xr_grapheme_cursor_init(XrGraphemeCursor *cursor, const uint8_t *data, size_t length) {
    XR_DCHECK(cursor != NULL, "grapheme cursor must not be NULL");
    XR_DCHECK(data != NULL || length == 0, "non-empty grapheme input must have data");
    cursor->data = data;
    cursor->length = length;
    cursor->offset = 0;
    cursor->state = 0;
    cursor->last_rule = XR_GRAPHEME_RULE_NONE;
}

bool xr_grapheme_cursor_next_traced(XrGraphemeCursor *cursor, XrByteRange *out,
                                    XrGraphemeTraceFn trace, void *context) {
    size_t start;
    size_t scan;
    XrUtf8Step decoded;
    uint8_t property;

    XR_DCHECK(cursor != NULL, "grapheme cursor must not be NULL");
    XR_DCHECK(out != NULL, "grapheme range must not be NULL");
    if (cursor->offset >= cursor->length)
        return false;

    start = cursor->offset;
    if (start == 0)
        emit_trace(trace, context, 0, true, XR_GRAPHEME_RULE_GB1);

    decoded = decode_valid_scalar(cursor->data + start, cursor->length - start);
    property = xr_unicode_grapheme_property_word(decoded.scalar);
    cursor->state = state_init(property);
    scan = start + decoded.consumed;

    /* Ordinary ASCII followed by ASCII always breaks. This hot path avoids
     * table lookups for the overwhelmingly common one-byte cluster case. */
    if (cursor->data[start] >= UINT8_C(0x20) && cursor->data[start] <= UINT8_C(0x7e) &&
        (scan == cursor->length || cursor->data[scan] < UINT8_C(0x80))) {
        XrGraphemeRule rule;
        if (scan == cursor->length) {
            rule = XR_GRAPHEME_RULE_GB2;
        } else {
            uint8_t next_property = xr_unicode_grapheme_property_word(cursor->data[scan]);
            bool is_break = boundary_decision(cursor->state, next_property, &rule);
            XR_DCHECK(is_break, "ordinary ASCII followed by ASCII must break");
            (void) is_break;
        }
        emit_trace(trace, context, scan, true, rule);
        cursor->offset = scan;
        cursor->last_rule = rule;
        out->start = start;
        out->end = scan;
        return true;
    }

    while (scan < cursor->length) {
        XrGraphemeRule rule;
        bool is_break;
        decoded = decode_valid_scalar(cursor->data + scan, cursor->length - scan);
        property = xr_unicode_grapheme_property_word(decoded.scalar);
        is_break = boundary_decision(cursor->state, property, &rule);
        emit_trace(trace, context, scan, is_break, rule);
        if (is_break) {
            cursor->offset = scan;
            cursor->last_rule = rule;
            out->start = start;
            out->end = scan;
            return true;
        }
        cursor->state = state_update(cursor->state, property);
        scan += decoded.consumed;
    }

    emit_trace(trace, context, cursor->length, true, XR_GRAPHEME_RULE_GB2);
    cursor->offset = cursor->length;
    cursor->last_rule = XR_GRAPHEME_RULE_GB2;
    out->start = start;
    out->end = cursor->length;
    return true;
}

bool xr_grapheme_cursor_next(XrGraphemeCursor *cursor, XrByteRange *out) {
    return xr_grapheme_cursor_next_traced(cursor, out, NULL, NULL);
}

const char *xr_grapheme_rule_name(XrGraphemeRule rule) {
    static const char *const names[] = {
        "none", "GB1", "GB2",  "GB3",  "GB4",  "GB5",  "GB6",       "GB7",
        "GB8",  "GB9", "GB9a", "GB9b", "GB9c", "GB11", "GB12/GB13", "GB999",
    };
    if ((unsigned) rule >= sizeof(names) / sizeof(names[0]))
        return "invalid";
    return names[rule];
}
