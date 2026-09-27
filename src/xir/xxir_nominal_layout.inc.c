/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_layout.inc.c - Bounded direct nominal field dependency proof
 *
 * KEY CONCEPT:
 *   Active paths reject infinite values; completed nodes permit shared fields.
 */
typedef struct NominalLayoutFrame { uint32_t index, next; } NominalLayoutFrame;
static XrXirStatus nominal_field_visibility(const XrXirTypes *types, XrXirBudget *budget) {
    const XrXirNominalTable *table = types->nominals;
    if (!table->declarations) return XR_XIR_OK;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        for (uint32_t f = 0; f < d->field_count; ++f) {
            if (!nominal_charge(budget, 0, 1)) return XR_XIR_BUDGET;
            const XrXirTypeNode *node = xr_xir_type_node(types, d->fields[f].type);
            if (!node || node->kind != XR_XIR_TYPE_NOMINAL) continue;
            const XrXirNominalDeclaration *target = &table->declarations[node->nominal.declaration];
            if (target->exported) continue;
            bool same = false;
            XrXirStatus status = nominal_same_name(d->module, target->module, budget, &same);
            if (status != XR_XIR_OK) return status;
            if (!same) return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus nominal_layout_verify(const XrXirTypes *types, XrXirBudget *budget) {
    if (!types->nominals) return XR_XIR_OK;
    XrXirStatus status = nominal_field_visibility(types, budget);
    if (status != XR_XIR_OK || !types->count) return status;
    uint64_t stack_bytes = (uint64_t) types->count * sizeof(NominalLayoutFrame);
    if (!nominal_charge(budget, stack_bytes + types->count, types->count)) return XR_XIR_BUDGET;
    NominalLayoutFrame *stack = xr_malloc((size_t) (stack_bytes + types->count));
    if (!stack) return XR_XIR_OUT_OF_MEMORY;
    unsigned char *state = (unsigned char *) stack + (size_t) stack_bytes;
    memset(state, 0, types->count);
    for (uint32_t root = 0; root < types->count && status == XR_XIR_OK; ++root) {
        if (types->nodes[root].kind != XR_XIR_TYPE_NOMINAL || state[root]) continue;
        uint32_t depth = 1; stack[0] = (NominalLayoutFrame) {root, 0}; state[root] = 1;
        while (depth && status == XR_XIR_OK) {
            NominalLayoutFrame *frame = &stack[depth - 1];
            const XrXirNominalType *node = &types->nodes[frame->index].nominal;
            const XrXirNominalDeclaration *d = !node->field_count && types->nominals->declarations ?
                &types->nominals->declarations[node->declaration] : NULL;
            uint32_t count = d ? d->field_count : node->field_count;
            if (frame->next == count) { state[frame->index] = 2; --depth; continue; }
            if (!nominal_charge(budget, 0, 1)) { status = XR_XIR_BUDGET; break; }
            XrXirType field = d ? d->fields[frame->next].type : node->fields[frame->next];
            ++frame->next;
            const XrXirTypeNode *child = xr_xir_type_node(types, field);
            if (!child || child->kind != XR_XIR_TYPE_NOMINAL) continue;
            uint32_t index = (uint32_t) field - XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (state[index] == 1) { status = XR_XIR_BAD_TYPE; break; }
            if (state[index] == 2) continue;
            if (depth >= types->count) { status = XR_XIR_BAD_TYPE; break; }
            state[index] = 1; stack[depth++] = (NominalLayoutFrame) {index, 0};
        }
    }
    xr_free(stack); return status;
}
