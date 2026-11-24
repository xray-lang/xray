/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_cycles.c - Strong ownership edges of specialized types
 */
#include "xxir_class_cycles.h"
#include "xxir_file_stat_native.h"
#include "xxir_internal.h"
#include "xxir_nominal.h"
#include "xxir_receive_control.h"
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"

struct XrXirClassCycles {
    const XrXirArtifact *artifact;
};

typedef struct ClassCycleVertex {
    uint32_t number, low;
    bool active, self_edge, unknown;
} ClassCycleVertex;

typedef struct ClassCycleFrame {
    uint32_t index, next, count;
} ClassCycleFrame;

typedef struct ClassCycleWalk {
    const XrXirTypes *types;
    const uint8_t *derived;
    ClassCycleVertex *vertices;
    ClassCycleFrame *frames;
    uint32_t *active;
    uint8_t *kinds;
    uint32_t numbered, depth, active_count;
} ClassCycleWalk;

static bool class_cycle_leaf(XrXirType type) {
    return type == XR_XIR_UNIT || type == XR_XIR_NEVER || type == XR_XIR_BOOL ||
        type == XR_XIR_RUNE || type == XR_XIR_STRING || xr_xir_type_is_number(type);
}

static uint32_t class_cycle_edge_count(const XrXirTypes *types, uint32_t index, bool *unknown) {
    const XrXirTypeNode *node = &types->nodes[index];
    if (node->parameter_span) {
        *unknown = true;
        return 0;
    }
    switch (node->kind) {
    case XR_XIR_TYPE_ARRAY:
    case XR_XIR_TYPE_SET:
    case XR_XIR_TYPE_NULLABLE:
    case XR_XIR_TYPE_CELL:
    case XR_XIR_TYPE_ATOMIC:
        return 1;
    case XR_XIR_TYPE_FIXED_ARRAY:
        return node->parameter_count != 0;
    case XR_XIR_TYPE_MAP:
        return 2;
    case XR_XIR_TYPE_TUPLE:
    case XR_XIR_TYPE_DATA_UNION:
        if (node->parameters) return node->parameter_count;
        break;
    case XR_XIR_TYPE_NOMINAL: {
        const XrXirNominalTable *table = types->nominals;
        uint32_t declaration = node->nominal.declaration;
        if (!table || declaration >= table->count || (!table->declarations && !table->identities)) break;
        const XrXirNominalDeclaration *definition = table->declarations ? &table->declarations[declaration] : NULL;
        const XrXirNominalIdentity *identity = definition ? NULL : &table->identities[declaration];
        uint32_t fields = definition ? definition->field_count : identity->field_count;
        uint32_t native = definition ? definition->native.native_id : identity->native.native_id;
        /* File storage owns a host descriptor and has no guest reference edges. */
        if (native == XR_NATIVE_DECLARATION_FILE_STORAGE && !fields && !node->nominal.field_count) return 0;
        if (native == XR_NATIVE_DECLARATION_FILE_STAT &&
            xir_file_stat_type(types, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + index))) return fields;
        if (!native && node->nominal.field_count == fields && (!fields || node->nominal.fields)) return fields;
        break;
    }
    default:
        break;
    }
    /* A callable or opaque value may own an arbitrary class through its payload. */
    *unknown = true;
    return 0;
}

static XrXirType class_cycle_edge(const XrXirTypeNode *node, uint32_t edge) {
    switch (node->kind) {
    case XR_XIR_TYPE_NOMINAL: return node->nominal.fields[edge];
    case XR_XIR_TYPE_TUPLE:
    case XR_XIR_TYPE_DATA_UNION: return node->parameters[edge].type;
    case XR_XIR_TYPE_MAP: return edge ? node->result : node->element;
    default: return node->element;
    }
}

static void class_cycle_enter(ClassCycleWalk *walk, uint32_t index) {
    ClassCycleVertex *vertex = &walk->vertices[index];
    vertex->number = vertex->low = ++walk->numbered;
    vertex->active = true;
    uint32_t count = class_cycle_edge_count(walk->types, index, &vertex->unknown);
    walk->frames[walk->depth++] = (ClassCycleFrame){index, 0, count};
    walk->active[walk->active_count++] = index;
}

static void class_cycle_finish_component(ClassCycleWalk *walk, uint32_t root) {
    uint32_t end = walk->active_count, member;
    bool unknown = false;
    do {
        member = walk->active[--walk->active_count];
        ClassCycleVertex *vertex = &walk->vertices[member];
        vertex->active = false;
        unknown |= vertex->unknown;
    } while (member != root);
    bool cyclic = end - walk->active_count > 1 || walk->vertices[root].self_edge;
    XrXirClassCycleKind kind = unknown ? XR_XIR_CLASS_CYCLE_UNKNOWN :
        cyclic ? XR_XIR_CLASS_CYCLE_POSSIBLE : XR_XIR_CLASS_CYCLE_ACYCLIC;
    for (uint32_t n = walk->active_count; n < end; ++n) walk->kinds[walk->active[n]] = (uint8_t)kind;
}

static XrXirStatus class_cycle_walk(ClassCycleWalk *walk, XrCompileRequestControl *control) {
    uint32_t steps = 0;
    for (uint32_t root = 0; root < walk->types->count; ++root) {
        if (walk->vertices[root].number) continue;
        class_cycle_enter(walk, root);
        while (walk->depth) {
            XrXirStatus status = xir_receive_step(control, &steps);
            if (status != XR_XIR_OK) return status;
            ClassCycleFrame *frame = &walk->frames[walk->depth - 1];
            ClassCycleVertex *vertex = &walk->vertices[frame->index];
            if (frame->next == frame->count) {
                if (vertex->low == vertex->number) class_cycle_finish_component(walk, frame->index);
                if (--walk->depth) {
                    ClassCycleVertex *parent = &walk->vertices[walk->frames[walk->depth - 1].index];
                    if (vertex->low < parent->low) parent->low = vertex->low;
                    if (!vertex->active && walk->kinds[frame->index] == XR_XIR_CLASS_CYCLE_UNKNOWN)
                        parent->unknown = true;
                }
                continue;
            }

            XrXirType child = class_cycle_edge(&walk->types->nodes[frame->index], frame->next++);
            if (class_cycle_leaf(child)) continue;
            const XrXirTypeNode *node = xr_xir_type_node(walk->types, child);
            /* A reference typed as a base may hold a derived allocation. Until
             * this pass has an exact derived set, conservatively keep that edge open. */
            if (!node || (walk->derived && xr_xir_type_is_class(walk->types, child) &&
                walk->derived[node->nominal.declaration])) {
                vertex->unknown = true;
                continue;
            }
            uint32_t index = (uint32_t)child - XR_XIR_CONSTRUCTED_TYPE_BASE;
            ClassCycleVertex *next = &walk->vertices[index];
            if (!next->number) class_cycle_enter(walk, index);
            else if (next->active) {
                vertex->self_edge |= index == frame->index;
                if (next->number < vertex->low) vertex->low = next->number;
            } else if (walk->kinds[index] == XR_XIR_CLASS_CYCLE_UNKNOWN) {
                vertex->unknown = true;
            }
        }
    }
    return xir_receive_poll(control);
}

XR_FUNC XrXirStatus xr_xir_class_cycles_build(const XrXirArtifact *artifact, XrXirClassCycles **empty,
    XrCompileRequestControl *control) {
    if (!artifact || !empty || *empty) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (module->stage != XR_XIR_CHECKED || module->linkage_kind != XR_XIR_PROGRAM ||
        !xir_effect_evidence_is_instance(module)) return XR_XIR_BAD_STAGE;
    XrXirStatus status = xir_receive_poll(control);
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    const XrXirTypes *types = module->types;
    uint32_t count = types ? types->count : 0;
    const XrXirNominalTable *nominals = types ? types->nominals : NULL;
    uint32_t declarations = nominals ? nominals->count : 0;
    if ((count && !types->nodes) || (declarations && !nominals->declarations && !nominals->identities))
        return XR_XIR_BAD_STRUCTURE;
    XrXirClassCycles *cycles = xir_compile_calloc(context, 1, sizeof(*cycles) + count, &status);
    ClassCycleVertex *vertices = xir_compile_calloc(context, count, sizeof(*vertices), &status);
    ClassCycleFrame *frames = xir_compile_calloc(context, count, sizeof(*frames), &status);
    uint32_t *active = xir_compile_calloc(context, count, sizeof(*active), &status);
    uint8_t *derived = xir_compile_calloc(context, declarations, sizeof(*derived), &status);
    if (status == XR_XIR_OK) {
        uint32_t steps = 0;
        for (uint32_t n = 0; status == XR_XIR_OK && n < declarations; ++n) {
            uint32_t parent = nominals->declarations ? nominals->declarations[n].parent : nominals->identities[n].parent;
            if (parent > declarations) status = XR_XIR_BAD_STRUCTURE;
            else if (parent) derived[parent - 1] = 1;
            if (status == XR_XIR_OK) status = xir_receive_step(control, &steps);
        }
        ClassCycleWalk walk = {.types=types, .derived=derived, .vertices=vertices,
            .frames=frames, .active=active, .kinds=(uint8_t *)(cycles + 1)};
        if (status == XR_XIR_OK && count) status = class_cycle_walk(&walk, control);
    }
    xr_compile_resources_free(derived);
    xr_compile_resources_free(active);
    xr_compile_resources_free(frames);
    xr_compile_resources_free(vertices);
    if (status == XR_XIR_OK) {
        cycles->artifact = artifact;
        *empty = cycles;
    } else xr_compile_resources_free(cycles);
    return status;
}

XR_FUNC XrXirStatus xr_xir_class_cycles_query(const XrXirClassCycles *cycles, const XrXirArtifact *artifact,
    XrXirType type, XrXirClassCycleKind *output) {
    if (!cycles || cycles->artifact != artifact || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *types = xr_xir_compile_artifact_module(artifact)->types;
    if (!xr_xir_type_is_class(types, type)) return XR_XIR_BAD_TYPE;
    const uint8_t *kinds = (const uint8_t *)(cycles + 1);
    *output = (XrXirClassCycleKind)kinds[(uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE];
    return XR_XIR_OK;
}

XR_FUNC void xr_xir_class_cycles_free(XrXirClassCycles *cycles) {
    xr_compile_resources_free(cycles);
}
