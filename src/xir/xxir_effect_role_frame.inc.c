/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_frame.inc.c - Current live ranges for role replay
 *
 * A ledger containment query never reads a proposed payload address. These
 * checks establish current geometry, not semantic permission or a reservation.
 */
typedef struct EffectBodyFrame {
    const XrXirCompileContext *work;
    const EffectBodyMaterial *material;
    uint32_t functions;
} EffectBodyFrame;

static XrXirStatus effect_role_range(const XrXirCompileContext *work,
    const void *memory,uint64_t count,size_t width) {
    if (!count) return XR_XIR_OK;
    if (!memory || !width || count>SIZE_MAX/width) return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus status=xr_compile_resources_extent(work->resources,memory,(size_t)count*width);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_role_material_frame(const XrXirCompileContext *work,
    const EffectBodyMaterial *material) {
    if (!xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;uint32_t depth=0;
    for (const EffectBodyMaterial *body=material;body;body=body->backbone) {
        if (depth++==3) return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,body,1,sizeof(*body));if (status!=XR_XIR_OK) return status;

        if (body->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
        if (body->overlay) {
            status=effect_role_range(work,body->overlay,1,sizeof(*body->overlay));
            if (status!=XR_XIR_OK) return status;
        }
    }
    return effect_body_material_shape(material)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
