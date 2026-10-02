/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_storage.inc.c - Checked inline nominal field storage layout
 *
 * KEY CONCEPT:
 *   One query computes shared children once and publishes complete offsets.
 */
typedef struct NominalStorageNode {
    XrXirLayout layout;
    uint32_t *offsets;
    uint32_t next, state, variant, maximum;
    uint32_t depth, owned_depth, tag_bytes;
} NominalStorageNode;
static bool nominal_storage_align(uint32_t size, uint32_t alignment, uint32_t *output) {
    if (!alignment || (alignment & (alignment - 1))) return false;
    uint32_t padding = (alignment - (size & (alignment - 1))) & (alignment - 1);
    if (size > UINT32_MAX - padding) return false;
    *output = size + padding; return true;
}
static XrXirStatus nominal_storage_walk(const XrXirTypes *types, const XrXirTarget *target,
    XrXirBudget *budget, NominalStorageNode *nodes, uint32_t *stack, uint32_t root_index) {
    uint32_t depth = 1;
    XrXirStatus status = XR_XIR_OK;
    stack[0] = root_index;
    nodes[root_index].state = 1; nodes[root_index].layout.alignment = 1;
    nodes[root_index].layout.size = types->nodes[root_index].kind == XR_XIR_TYPE_NULLABLE ? 1 : 0;
    nodes[root_index].depth = 1;
    while (depth && status == XR_XIR_OK) {
        if (!budget->work) { status = XR_XIR_BUDGET; break; }
        --budget->work;
        uint32_t index = stack[depth - 1];
        NominalStorageNode *current = &nodes[index];
        if (types->nodes[index].parameter_span) return XR_XIR_BAD_LAYOUT;
        const XrXirTypeNode *type_node = &types->nodes[index];
        bool nullable = type_node->kind == XR_XIR_TYPE_NULLABLE;
        const XrXirNominalType *nominal = &type_node->nominal;
        uint32_t field_count = nullable ? 1 : nominal->field_count;
        uint32_t declared = nullable ? 0 : types->nominals->declarations ?
            types->nominals->declarations[nominal->declaration].field_count :
            types->nominals->identities[nominal->declaration].field_count;
        if (nominal->field_count != declared) { status = XR_XIR_BAD_LAYOUT; break; }
        bool is_enum = xr_xir_type_is_enum(types, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + index));
        const XrXirNominalVariant *variants = NULL; uint32_t variant_count = 0;
        if (is_enum) {
            if (types->nominals->declarations) {
                const XrXirNominalDeclaration *d = &types->nominals->declarations[nominal->declaration];
                variants = d->variants; variant_count = d->variant_count;
            } else {
                const XrXirNominalIdentity *d = &types->nominals->identities[nominal->declaration];
                variants = d->variants; variant_count = d->variant_count;
            }
        }
        if (current->next == field_count) {
            if (nullable) current->tag_bytes = 1;
            if (is_enum) {
                uint32_t tag = variant_count == 1 ? 0 : variant_count <= 256 ? 1 : variant_count <= 65536 ? 2 : 4;
                current->tag_bytes = tag;
                uint32_t base = 0;
                if (!nominal_storage_align(tag, current->layout.alignment, &base) ||
                    current->maximum > UINT32_MAX - base) { status = XR_XIR_BAD_LAYOUT; break; }
                current->layout.size = base + current->maximum;
                if (tag > current->layout.alignment) current->layout.alignment = tag;
                if (current->offsets != NULL)
                    for (uint32_t f = 0; f < nominal->field_count; ++f) {
                        if (!budget->work) { status = XR_XIR_BUDGET; break; }
                        --budget->work; current->offsets[f] += base;
                    }
                if (status != XR_XIR_OK) break;
            }
            if (!nominal_storage_align(current->layout.size, current->layout.alignment, &current->layout.size)) {
                status = XR_XIR_BAD_LAYOUT; break;
            }
            current->state = 2; --depth; continue;
        }
        if (is_enum) {
            while (current->variant < variant_count && current->next >=
                variants[current->variant].field_begin + variants[current->variant].field_count) {
                if (!budget->work) { status = XR_XIR_BUDGET; break; }
                --budget->work; ++current->variant;
            }
            if (status != XR_XIR_OK) break;
            if (current->variant >= variant_count) { status = XR_XIR_BAD_LAYOUT; break; }
            if (current->next == variants[current->variant].field_begin) current->layout.size = 0;
        }
        XrXirType field = nullable ? type_node->element : nominal->fields[current->next];
        const XrXirTypeNode *child = xr_xir_type_node(types, field);
        XrXirLayout physical = {0};
        if (child && ((child->kind == XR_XIR_TYPE_NOMINAL && !xr_xir_type_is_class(types, field)) ||
                      child->kind == XR_XIR_TYPE_NULLABLE)) {
            uint32_t child_index = (uint32_t) field - XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (nodes[child_index].state == 1) { status = XR_XIR_BAD_LAYOUT; break; }
            if (!nodes[child_index].state) {
                if (depth >= types->count) { status = XR_XIR_BAD_LAYOUT; break; }
                nodes[child_index].state = 1; nodes[child_index].layout.alignment = 1;
                nodes[child_index].layout.size = child->kind == XR_XIR_TYPE_NULLABLE ? 1 : 0;
                nodes[child_index].depth = 1;
                stack[depth++] = child_index; continue;
            }
            physical = nodes[child_index].layout;
            if (nodes[child_index].depth >= current->depth) current->depth = nodes[child_index].depth + 1;
            if (nodes[child_index].owned_depth && nodes[child_index].owned_depth >= current->owned_depth)
                current->owned_depth = nodes[child_index].owned_depth + 1;
        } else {
            status = xr_xir_layout(types, field, target, XR_XIR_LAYOUT_STORAGE, &physical);
            if (status != XR_XIR_OK) break;
            if (xr_xir_type_is_owned(types, field) && !current->owned_depth) current->owned_depth = 1;
        }
        uint32_t offset = 0;
        if (!nominal_storage_align(current->layout.size, physical.alignment, &offset) ||
            physical.size > UINT32_MAX - offset) { status = XR_XIR_BAD_LAYOUT; break; }
        if (current->offsets != NULL) current->offsets[current->next] = offset;
        current->layout.size = offset + physical.size;
        if (current->layout.size > current->maximum) current->maximum = current->layout.size;
        if (physical.alignment > current->layout.alignment) current->layout.alignment = physical.alignment;
        ++current->next;
    }
    return status;
}
typedef struct InlineLayoutOutput {
    XrXirLayout *layout;
    uint32_t *field_offsets, field_count;
} InlineLayoutOutput;
static XrXirStatus inline_layout_query(const XrXirTypes *types, XrXirType type,
    const XrXirTarget *target, XrXirBudget *remaining, InlineLayoutOutput output) {
    XrXirLayout *layout=output.layout;
    uint32_t *field_offsets=output.field_offsets,field_count=output.field_count;
    if (!layout) return XR_XIR_BAD_LAYOUT;
    *layout = (XrXirLayout) {0, 0};
    if (!remaining || !target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        (field_count != 0) != (field_offsets != NULL)) return XR_XIR_BAD_LAYOUT;
    XrXirBudget budget = *remaining;
    XrXirStatus status = xr_xir_types_structure_verify(types, &budget);
    if (status != XR_XIR_OK) return status;
    const XrXirTypeNode *root = xr_xir_type_node(types, type);
    if (!root || (root->kind != XR_XIR_TYPE_NOMINAL && root->kind != XR_XIR_TYPE_NULLABLE) ||
        xr_xir_type_is_class(types,type) || root->parameter_span ||
        root->nominal.field_count != field_count) return XR_XIR_BAD_LAYOUT;
    uint64_t bytes = (uint64_t) types->count * (sizeof(NominalStorageNode) + sizeof(uint32_t)) +
        (uint64_t) field_count * sizeof(uint32_t);
    if (bytes > SIZE_MAX || bytes > budget.metadata_bytes || types->count > budget.work) return XR_XIR_BUDGET;
    budget.metadata_bytes -= bytes; budget.work -= types->count;
    NominalStorageNode *nodes = xr_calloc(1, (size_t) bytes);
    if (!nodes) return XR_XIR_OUT_OF_MEMORY;
    uint32_t *stack = (uint32_t *) (nodes + types->count), *offsets = stack + types->count;
    uint32_t root_index = (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE;
    nodes[root_index].offsets = field_count ? offsets : NULL;
    status = nominal_storage_walk(types, target, &budget, nodes, stack, root_index);
    if (status == XR_XIR_OK) {
        *layout = nodes[root_index].layout;
        if (field_count) memcpy(field_offsets, offsets, (size_t) field_count * sizeof(*offsets));
        *remaining = budget;
    }
    xr_free(nodes); return status;
}
XR_FUNC XrXirStatus xr_xir_nominal_layout(const XrXirTypes *types, XrXirType type,
    const XrXirTarget *target, XrXirBudget *remaining, XrXirLayout *layout,
    uint32_t *field_offsets, uint32_t field_count) {
    if (!xr_xir_type_is_nominal(types, type)) {
        if (layout) *layout = (XrXirLayout){0};
        return XR_XIR_BAD_LAYOUT;
    }
    return inline_layout_query(types, type, target, remaining,
        (InlineLayoutOutput){layout,field_offsets,field_count});
}
static XrXirStatus nullable_storage_layout(const XrXirTypes *types, XrXirType type,
    const XrXirTarget *target, XrXirLayout *layout) {
    XrXirBudget budget = xr_xir_default_budget();
    return inline_layout_query(types, type, target, &budget, (InlineLayoutOutput){layout,NULL,0});
}
