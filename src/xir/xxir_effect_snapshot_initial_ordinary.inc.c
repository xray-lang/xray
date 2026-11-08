/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * An ordinary view owns its changed metadata and retains only a completely
 * corresponding immutable initial body from this same compiler operation.
 */
static void effect_body_ordinary_overlay_free(EffectBodyOrdinaryOverlay *overlay) {
    if (!overlay) return;
    if (overlay->functions) for (uint32_t f=0;f<overlay->count;++f)
        xr_compile_resources_free((void *)overlay->functions[f].initial_roles);
    xr_compile_resources_free(overlay->ordinary_identities);
    xr_compile_resources_free(overlay->identities);xr_compile_resources_free(overlay->generics);
    xr_compile_resources_free(overlay->functions);xr_compile_resources_free(overlay);
}

/* NULL source generics and a real non-NULL empty ordinary table are different
 * representations. The new table is owned, never canonicalized into NULL. */

/* Bounds come from this current ordinary input through the original sole
 * advertisement builder. The initial TEMPLATE may have different CALL_BIND
 * advertisements, so its bound inventory is never copied as this overlay. */
