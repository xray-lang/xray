/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_feedback.c - Runtime type feedback for profile-guided compilation
 *
 * KEY CONCEPT:
 *   Provides allocation, analysis, and debug utilities for type feedback.
 *   The hot inline paths (xfb_record_arg/return) are in the header.
 *   This file contains cold-path helpers used by JIT/AOT consumers.
 *
 * WHY THIS DESIGN:
 *   - Always compiled (no XRAY_HAS_JIT guard) — profile is a shared resource
 *   - Slot type conversion bridges feedback bitmask → XrSlotType for JIT/AOT
 */

#include "xir_feedback.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xslot_type.h"
#include <stdio.h>

/* ========== Allocation ========== */

XirTypeFeedback *xfb_create(void) {
    XirTypeFeedback *fb = xr_calloc(1, sizeof(XirTypeFeedback));
    return fb;
}

void xfb_destroy(XirTypeFeedback *fb) {
    if (!fb) return;
    xr_free(fb);
}

/* ========== Analysis ========== */

bool xfb_is_monomorphic(uint8_t type_flags) {
    // Monomorphic = exactly one bit set (power of 2)
    return type_flags != 0 && (type_flags & (type_flags - 1)) == 0;
}

uint8_t xfb_to_slot_type(uint8_t type_flags) {
    // Convert single-type feedback to XrSlotType
    switch (type_flags) {
        case XFB_TYPE_INT:    return XR_SLOT_I64;
        case XFB_TYPE_FLOAT:  return XR_SLOT_F64;
        case XFB_TYPE_BOOL:   return XR_SLOT_BOOL;
        case XFB_TYPE_STRING: return XR_SLOT_PTR;
        case XFB_TYPE_OBJECT: return XR_SLOT_PTR;
        default:              return XR_SLOT_ANY;  // mixed or unknown
    }
}

/* ========== Debug ========== */

static const char *xfb_type_name(uint8_t flags) {
    if (flags == XFB_TYPE_NONE)   return "none";
    if (flags == XFB_TYPE_INT)    return "int";
    if (flags == XFB_TYPE_FLOAT)  return "float";
    if (flags == XFB_TYPE_BOOL)   return "bool";
    if (flags == XFB_TYPE_STRING) return "string";
    if (flags == XFB_TYPE_OBJECT) return "object";
    if (flags == XFB_TYPE_NULL)   return "null";
    return "mixed";
}

void xfb_dump(const XirTypeFeedback *fb, int nparams, const char *func_name) {
    if (!fb) {
        fprintf(stderr, "[feedback] %s: no profile\n", func_name ? func_name : "?");
        return;
    }

    fprintf(stderr, "[feedback] %s: samples=%u stable=%s\n",
            func_name ? func_name : "?",
            fb->sample_count,
            fb->stable ? "yes" : "no");

    for (int i = 0; i < nparams && i < XFB_MAX_PARAMS; i++) {
        fprintf(stderr, "  arg[%d]: %s (0x%02x)\n",
                i, xfb_type_name(fb->arg_types[i]), fb->arg_types[i]);
    }

    fprintf(stderr, "  return: %s (0x%02x)\n",
            xfb_type_name(fb->return_type), fb->return_type);
}
