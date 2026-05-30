/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pic.c - Polymorphic inline cache management
 *
 * Maintains per-call-site type → method caches for polymorphic
 * dispatch.  The PIC transitions through states:
 *   EMPTY → MONOMORPHIC → POLYMORPHIC → MEGAMORPHIC
 *
 * Once megamorphic, the PIC is frozen (no new entries) and the
 * call site uses slow-path vtable lookup exclusively.
 */

#include "xm_pic.h"
#include "../vm/xic_method.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/value/xchunk.h"
#include <string.h>

XR_FUNC void xm_pic_init(XmPic *pic) {
    if (!pic)
        return;
    memset(pic, 0, sizeof(XmPic));
    pic->state = XM_PIC_EMPTY;
}

XR_FUNC bool xm_pic_record(XmPic *pic, uint32_t type_id, uint32_t method_offset, void *code_ptr) {
    if (!pic || type_id == 0)
        return false;

    /* Check for existing entry. */
    for (uint8_t i = 0; i < pic->nentries; i++) {
        if (pic->entries[i].type_id == type_id) {
            pic->entries[i].code_ptr = code_ptr;
            pic->hit_count++;
            return true;
        }
    }

    /* Miss: try to add. */
    if (pic->state == XM_PIC_MEGAMORPHIC) {
        pic->miss_count++;
        return false;
    }

    if (pic->nentries >= XM_PIC_MAX_ENTRIES) {
        pic->state = XM_PIC_MEGAMORPHIC;
        pic->miss_count++;
        return false;
    }

    XmPicEntry *entry = &pic->entries[pic->nentries++];
    entry->type_id = type_id;
    entry->method_offset = method_offset;
    entry->code_ptr = code_ptr;

    if (pic->nentries == 1)
        pic->state = XM_PIC_MONOMORPHIC;
    else
        pic->state = XM_PIC_POLYMORPHIC;

    pic->miss_count++;
    return false;
}

XR_FUNC void *xm_pic_lookup(const XmPic *pic, uint32_t type_id) {
    if (!pic || type_id == 0)
        return NULL;

    for (uint8_t i = 0; i < pic->nentries; i++) {
        if (pic->entries[i].type_id == type_id)
            return pic->entries[i].code_ptr;
    }

    return NULL;
}

XR_FUNC void xm_pic_reset(XmPic *pic) {
    if (!pic)
        return;
    xm_pic_init(pic);
}

XR_FUNC void xm_pic_import_ic_method(XmPic *pic, const XrICMethod *ic) {
    if (!pic)
        return;
    xm_pic_init(pic);
    if (!ic || ic->count == 0)
        return;
    if (ic->is_megamorphic) {
        pic->state = XM_PIC_MEGAMORPHIC;
        return;
    }

    for (int i = 0; i < ic->count && pic->nentries < XM_PIC_MAX_ENTRIES; i++) {
        XrClass *klass = ic->entries[i].klass;
        XrMethod *method = ic->entries[i].method;
        if (!klass)
            continue;

        XmPicEntry *entry = &pic->entries[pic->nentries++];
        entry->type_id = (uint32_t) (uintptr_t) klass;
        entry->method_offset = 0;
        entry->code_ptr = NULL;
        if (method && method->type == XMETHOD_CLOSURE && method->as.closure &&
            method->as.closure->proto) {
            entry->code_ptr = method->as.closure->proto->jit_entry;
        }
    }

    if (pic->nentries == 0)
        pic->state = XM_PIC_EMPTY;
    else if (pic->nentries == 1)
        pic->state = XM_PIC_MONOMORPHIC;
    else
        pic->state = XM_PIC_POLYMORPHIC;
}
