/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_source_terminal.inc.c - Fresh read-only Source terminal replay
 *
 * KEY CONCEPT:
 *   An immutable owner supplies storage, never a permission exemption. Each
 *   use checks physical inputs, closed equations and independent outcomes.
 *   Temporary type and invocation scratch dies before the final pure checks.
 */
/* Drop every borrowed constructor pointer before freeing the backbone's
 * real scratch. The owning immutable descriptors remain at the same addresses. */

/* Only the completed private constructor calls this. All real scratch frees
 * precede the last current-Input comparison and the sole public ownership move. */

/* Public formulas and terminal views borrow the sole Result. Exact alias
 * checks establish destruction provenance only; its stored records are
 * independently enumerated from the closed current equations below. */

/* Verification borrows the actual Final and its sole immutable Input.
 * Request scratch contains only the fresh incoming index; no construction
 * owner, queue or copied semantic inventory participates in this proof. */
/* Constructor relations grant no final admission. The terminal inventory
 * pass checks the sole finite source relation after all cleanup callbacks. */

/* Control and Error consume independent Final evidence after all earlier
 * receiving cleanup callbacks. Neither domain reconstructs an Error Graph. */

/* This terminal role pass follows every receiving free callback. It has
 * no mutable graph or scratch allocation and cannot retain a passed receipt. */

/* The semantic owner is already closed. External artifact verification scans
 * the input independently before checking this ownership association. */
static XrXirStatus effect_source_terminal_current(const XrXirCompileContext *work,
    const XrXirModule *source, const XrXirEffects *effects) {
    return effect_source_body_current(work, source, effects);
}

static XrXirStatus effect_source_input_terminal(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    return effect_source_body_current(work,source,effects);
}
static XrXirStatus effect_source_raw_terminal(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    if(!source || source->provenance) return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,source,effects);
}

static XrXirStatus effect_source_receiving_terminal(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects) {
    if(!module || !module->provenance || !xir_effect_kind_is_closed(module->provenance->kind))
        return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,module,effects);
}

static XrXirStatus effect_source_copy_match(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects) {
    return effect_source_body_current(work,module,effects);
}
static XrXirStatus effect_source_copy_terminal(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects) {
    if(!module || module->provenance) return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,module,effects);
}
