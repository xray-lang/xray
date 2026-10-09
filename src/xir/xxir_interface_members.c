/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interface_members.c - Bounded inherited application consistency
 *
 * KEY CONCEPT:
 *   Composed substitutions use the existing structural type matcher; original
 *   declarations keep every obligation even when member lookup can merge names.
 */
#include "xxir_interface_members.h"
#include "xxir_compile_memory.h"
#include "xxir_types.h"
#include "../base/xmalloc.h"
#include <string.h>

typedef struct MemberMemory { struct MemberMemory *next; } MemberMemory;
typedef struct MemberApplication {
    struct MemberApplication *next;
    uint32_t declaration, count, index;
    XrXirType *arguments;
} MemberApplication;
typedef struct MemberRequirement {
    struct MemberRequirement *next;
    XrXirInterfaceRequirement value;
} MemberRequirement;
typedef struct MemberTypeValue {
    struct MemberTypeValue *next;
    XrXirType source, result;
} MemberTypeValue;
typedef struct MemberTypeMap { MemberTypeValue *values; } MemberTypeMap;
typedef struct MemberTypeWalk {
    struct MemberTypeWalk *parent;
    XrXirType source;
    uint32_t next;
} MemberTypeWalk;
typedef struct MemberContext {
    const XrXirInterfaceTable *table;
    const XrXirTypes *source, *base;
    XrXirTypes types;
    uint32_t capacity, root_count, ambient_count;
    bool nodes_owned;
    XrXirType *root_arguments;
    MemberMemory *memory;
    MemberApplication *applications, *tail;
    MemberRequirement *requirements;
    uint32_t application_count, requirement_count;
    XrXirCompileContext remaining;
    XrXirStatus status;
} MemberContext;
struct XrXirInterfaceClosure {
    MemberMemory *memory;
    XrXirTypes types;
    XrXirInterfaceApplication *applications;
    XrXirInterfaceRequirement *requirements;
    uint32_t application_count, requirement_count;
};

static bool member_work(MemberContext *c, uint64_t work) {
    if (c->status != XR_XIR_OK) return false;
    if (!xir_compile_work(&c->remaining, work)) { c->status = XR_XIR_BUDGET; return false; }
     return true;
}
static void *member_reserve(MemberContext *c, uint64_t count, size_t size, bool clear) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!count || c->status != XR_XIR_OK) return NULL;
    if (count > (SIZE_MAX - sizeof(MemberMemory)) / size ||
(sizeof(MemberMemory) + count * size > SIZE_MAX)) {
        c->status = XR_XIR_BUDGET; return NULL;
    }
    size_t bytes = sizeof(MemberMemory) + (size_t)count * size;
    MemberMemory *memory = clear ? xir_compile_calloc(&c->remaining, 1, bytes, &allocation_status) :
        xir_compile_alloc(&c->remaining, bytes, &allocation_status);
    if (!memory) { c->status = allocation_status; return NULL; }

    memory->next = c->memory; c->memory = memory; return memory + 1;
}
static void *member_alloc(MemberContext *c, uint64_t count, size_t size) {
    return member_reserve(c, count, size, true);
}
static void member_dispose(MemberContext *c) {
    while (c->memory) { MemberMemory *next = c->memory->next; xr_compile_resources_free(c->memory); c->memory = next; }
}
static bool member_identity(MemberContext *c, uint32_t count) {
    if (count > XR_XIR_TYPE_PARAMETER_LIMIT-XR_XIR_TYPE_PARAMETER_BASE) {
        c->status = XR_XIR_BAD_TYPE; return false;
    }
    if (count <= c->root_count) return true;
    XrXirType *arguments = member_alloc(c,count,sizeof(*arguments));
    if (!arguments || !member_work(c,count)) return false;
    for (uint32_t p = 0; p < count; ++p) arguments[p] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    c->root_arguments = arguments; c->root_count = count; return true;
}
/* Distinct pool views request structural comparison even for closed nodes.
 * Root parameters retain their declaration context through identity arguments. */
static bool member_equal(MemberContext *c, XrXirType a, XrXirType b) {
    if (!member_work(c, 1)) return false;
    if (a == b) return true;
    XrXirTypes from = c->types;
    XrXirCompileContext match = c->remaining;
    XrXirStatus status = xr_xir_compile_type_substitution_matches_between(&match, &from, &c->types, c->root_arguments, c->root_count, a, b);

    if (status != XR_XIR_OK && status != XR_XIR_BAD_TYPE) c->status = status;
    return status == XR_XIR_OK;
}
static bool member_ready(MemberContext *c, const MemberApplication *app,
    const MemberTypeMap *map, XrXirType type, XrXirType *result) {
    if (!member_work(c, 1)) return false;
    uint32_t id = (uint32_t)type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = id - XR_XIR_TYPE_PARAMETER_BASE;
        if (index >= app->count) { c->status = XR_XIR_BAD_TYPE; return false; }
        *result = app->arguments[index]; return true;
    }
    const XrXirTypeNode *node = xr_xir_type_node(c->source, type);
    if (!node || (!node->parameter_span && c->source == c->base)) { *result = type; return true; }
    for (MemberTypeValue *value = map->values; value && member_work(c, 1); value = value->next)
        if (value->source == type) { *result = value->result; return true; }
    return false;
}
static XrXirType member_remap(MemberContext *c, const MemberApplication *app,
    const MemberTypeMap *map, XrXirType type) {
    XrXirType result = XR_XIR_UNIT;
    if (!member_ready(c, app, map, type, &result) && c->status == XR_XIR_OK)
        c->status = XR_XIR_BAD_TYPE;
    return result;
}
static XrXirType member_node(MemberContext *c, XrXirTypeNode node) {
    uint32_t limit = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (c->types.count == limit) { c->status = XR_XIR_BUDGET; return XR_XIR_UNIT; }
    if (c->types.count == c->capacity) {
        uint32_t capacity = c->capacity > limit / 2 ? limit : c->capacity ? c->capacity * 2 : 8;
        /* Materialize the borrowed prefix with a small tail for new descriptors. */
        if (!c->nodes_owned && c->capacity) {
            uint32_t tail = limit - c->capacity < 8 ? limit - c->capacity : 8;
            capacity = c->capacity + tail;
        }
        /* The visible prefix is copied below and each new descriptor is
         * assigned in full before count advances. Spare capacity is unread. */
        XrXirTypeNode *nodes = member_reserve(c, capacity, sizeof(*nodes), false);
        if (!nodes || !member_work(c, (uint64_t)c->types.count * sizeof(*nodes))) return XR_XIR_UNIT;
        if (c->types.count) memcpy(nodes, c->types.nodes, c->types.count * sizeof(*nodes));
        c->types.nodes = nodes; c->capacity = capacity; c->nodes_owned = true;
    }
    if (!member_work(c, sizeof(node))) return XR_XIR_UNIT;
    ((XrXirTypeNode *)c->types.nodes)[c->types.count] = node;
    return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + c->types.count++);
}
static void member_span(MemberContext *c, XrXirTypeNode *node, XrXirType child) {
    uint32_t span = xr_xir_type_span(&c->types, child);
    if (span > node->parameter_span) node->parameter_span = span;
}
/* A complete descent may leave the immutable source descriptor unchanged.
 * Only identical pool IDs preserve its fields, flags and parameter modes. */
static bool member_reuses_node(MemberContext *c, const MemberApplication *app,
    const MemberTypeMap *map, const XrXirTypeNode *node) {
    if (!member_work(c,1) || c->source != c->base) return false;
    uint32_t count = node->kind == XR_XIR_TYPE_CALLABLE ? node->parameter_count + 1 :
        node->kind == XR_XIR_TYPE_NOMINAL ? node->nominal.argument_count :
        node->kind == XR_XIR_TYPE_TUPLE ? node->parameter_count : 1;
    uint32_t span = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!member_work(c,1)) return false;
        XrXirType original = node->element;
        if (node->kind == XR_XIR_TYPE_CALLABLE)
            original = i == node->parameter_count ? node->result : node->parameters[i].type;
        else if (node->kind == XR_XIR_TYPE_NOMINAL) original = node->nominal.arguments[i];
        else if (node->kind == XR_XIR_TYPE_TUPLE) original = node->parameters[i].type;
        XrXirType actual = member_remap(c,app,map,original);
        if (c->status != XR_XIR_OK || actual != original) return false;
        uint32_t child_span = xr_xir_type_span(&c->types,actual);
        if (child_span > span) span = child_span;
    }
    return span == node->parameter_span && c->status == XR_XIR_OK;
}
static XrXirType member_substitute_node(MemberContext *c, const MemberApplication *app,
    const MemberTypeMap *map, XrXirTypeNode node) {
    node.parameter_span = 0;
    if (node.kind == XR_XIR_TYPE_CALLABLE || node.kind == XR_XIR_TYPE_TUPLE) {
        XrXirCallableParameter *parameters = member_alloc(c, node.parameter_count, sizeof(*parameters));
        if (node.parameter_count && !parameters) return XR_XIR_UNIT;
        for (uint32_t p = 0; p < node.parameter_count && c->status == XR_XIR_OK; ++p) {
            parameters[p] = node.parameters[p];
            parameters[p].type = member_remap(c, app, map, parameters[p].type);
            member_span(c, &node, parameters[p].type);
        }
        node.parameters = parameters;
        if (node.kind == XR_XIR_TYPE_CALLABLE) {
            node.result = member_remap(c, app, map, node.result);
            member_span(c, &node, node.result);
        }
    } else if (node.kind == XR_XIR_TYPE_NOMINAL) {
        XrXirType *arguments = member_alloc(c, node.nominal.argument_count, sizeof(*arguments));
        if (node.nominal.argument_count && !arguments) return XR_XIR_UNIT;
        for (uint32_t a = 0; a < node.nominal.argument_count && c->status == XR_XIR_OK; ++a) {
            arguments[a] = member_remap(c, app, map, node.nominal.arguments[a]);
            member_span(c, &node, arguments[a]);
        }
        node.nominal.arguments = arguments;
        /* Equality uses the nominal declaration and arguments, not field layout. */
        node.nominal.fields = NULL; node.nominal.field_count = 0;
    } else {
        node.element = member_remap(c, app, map, node.element); member_span(c, &node, node.element);
    }
    return c->status == XR_XIR_OK ? member_node(c, node) : XR_XIR_UNIT;
}
static MemberTypeWalk *member_type_push(MemberContext *c, MemberTypeWalk *parent, XrXirType type) {
    MemberTypeWalk *frame = member_alloc(c, 1, sizeof(*frame));
    if (frame) *frame = (MemberTypeWalk){parent,type,0};
    return frame;
}
/* Only referenced abstract nodes enter this sparse substitution cache. Closed
 * descriptors stay in the original pool; every descent follows a smaller ID. */
static XrXirType member_resolve(MemberContext *c, const MemberApplication *app,
    MemberTypeMap *map, XrXirType type) {
    XrXirType result = XR_XIR_UNIT;
    if (member_ready(c,app,map,type,&result)) return result;
    if (c->status != XR_XIR_OK) return XR_XIR_UNIT;
    MemberTypeWalk *frame = member_type_push(c,NULL,type);
    while (frame && member_work(c,1)) {
        const XrXirTypeNode *node = xr_xir_type_node(c->source,frame->source);
        if (!node) { c->status = XR_XIR_BAD_TYPE; break; }
        uint32_t components = node->kind == XR_XIR_TYPE_NOMINAL ? node->nominal.argument_count :
            node->kind == XR_XIR_TYPE_CALLABLE ? node->parameter_count + 1 :
            node->kind == XR_XIR_TYPE_TUPLE ? node->parameter_count : 1;
        if (frame->next == components) {
            result = member_reuses_node(c,app,map,node) ? frame->source :
                member_substitute_node(c,app,map,*node);
            MemberTypeValue *value = member_alloc(c,1,sizeof(*value));
            if (!value) break;
            *value = (MemberTypeValue){map->values,frame->source,result}; map->values = value;
            frame = frame->parent; continue;
        }
        XrXirType child = node->element;
        if (node->kind == XR_XIR_TYPE_NOMINAL) child = node->nominal.arguments[frame->next];
        if (node->kind == XR_XIR_TYPE_TUPLE) child = node->parameters[frame->next].type;
        if (node->kind == XR_XIR_TYPE_CALLABLE)
            child = frame->next < node->parameter_count ? node->parameters[frame->next].type : node->result;
        if (member_ready(c,app,map,child,&result)) { ++frame->next; continue; }
        if (c->status != XR_XIR_OK) break;
        if ((uint32_t)child >= (uint32_t)frame->source) { c->status = XR_XIR_BAD_TYPE; break; }
        frame = member_type_push(c,frame,child);
    }
    return c->status == XR_XIR_OK ? member_remap(c,app,map,type) : XR_XIR_UNIT;
}
static void member_application(MemberContext *c, uint32_t declaration, XrXirType *arguments) {
    uint32_t count = c->table->declarations[declaration].parameter_count;
    for (MemberApplication *a = c->applications; a && member_work(c, 1); a = a->next) {
        if (a->declaration != declaration) continue;
        bool same = true;
        for (uint32_t p = 0; p < count && same; ++p) same = member_equal(c, a->arguments[p], arguments[p]);
        if (same) return;
    }
    MemberApplication *app = member_alloc(c, 1, sizeof(*app));
    if (!app) return;
    if (c->application_count == UINT32_MAX) { c->status = XR_XIR_BUDGET; return; }
    app->declaration = declaration; app->count = count; app->arguments = arguments;
    app->index = c->application_count++;
    if (c->tail) c->tail->next = app; else c->applications = app;
    c->tail = app;
}
static void member_roots(MemberContext *c, const XrXirInterfaceApplication *roots, uint32_t count);
static bool member_condition_contains(MemberContext *c, XrXirConstraint facts, XrXirConstraint required) {
    if (!member_work(c,1) || (facts.markers & required.markers) != required.markers) return false;
    MemberContext parents = {0}; parents.table = c->table; parents.source = c->source;
    XrXirTypes input = c->types; parents.base = &input; parents.remaining = c->remaining;
    parents.ambient_count = c->root_count;
    member_roots(&parents,facts.interfaces,facts.interface_count);
    bool compatible = parents.status == XR_XIR_OK;
    for (uint32_t r = 0; compatible && r < required.interface_count; ++r) {
        const XrXirInterfaceApplication *wanted = &required.interfaces[r]; bool found = false;
        for (MemberApplication *a = parents.applications; a && member_work(&parents,1); a = a->next) {
            if (a->declaration != wanted->declaration || a->count != wanted->argument_count) continue;
            bool same = true;
            for (uint32_t p = 0; same && p < a->count; ++p) {
                XrXirCompileContext match = parents.remaining;
                XrXirStatus status = xr_xir_compile_type_substitution_matches_between(&match, &parents.types, &c->types, c->root_arguments, c->root_count, a->arguments[p], wanted->arguments[p]);

                if (status != XR_XIR_OK && status != XR_XIR_BAD_TYPE) parents.status = status;
                same = status == XR_XIR_OK;
            }
            if (same) { found = true; break; }
        }
        compatible = found && parents.status == XR_XIR_OK;
    }

    if (parents.status != XR_XIR_OK) c->status = parents.status;
    member_dispose(&parents); return compatible && c->status == XR_XIR_OK;
}
static XrXirConstraint member_constraint(MemberContext *c, const MemberApplication *substitution,
    MemberTypeMap *map, XrXirConstraint input) {
    XrXirConstraint output = {input.markers,NULL,input.interface_count};
    XrXirInterfaceApplication *apps = member_alloc(c,input.interface_count,sizeof(*apps));
    output.interfaces = apps;
    for (uint32_t i = 0; i < input.interface_count && member_work(c,1); ++i) {
        apps[i] = input.interfaces[i];
        XrXirType *arguments = member_alloc(c,apps[i].argument_count,sizeof(*arguments));
        for (uint32_t a = 0; a < apps[i].argument_count && member_work(c,1); ++a)
            arguments[a] = member_resolve(c,substitution,map,apps[i].arguments[a]);
        apps[i].arguments = arguments;
    }
    return output;
}
static void member_requirement(MemberContext *c, const MemberApplication *app,
    uint32_t member, XrXirType signature, const XrXirConstraint *constraints) {
    const XrXirInterfaceMethod *method = &c->table->declarations[app->declaration].methods[member];
    for (MemberRequirement *r = c->requirements; r; r = r->next) {
        if (!member_work(c, (uint64_t)method->name.length + 1)) return;
        if (r->value.name.length != method->name.length ||
            memcmp(r->value.name.bytes, method->name.bytes, r->value.name.length)) continue;
        bool compatible = r->value.receiver == method->receiver &&
            r->value.own_parameter_count == method->own_parameter_count && member_equal(c,r->value.signature,signature);
        for (uint32_t p = 0; compatible && p < method->own_parameter_count; ++p)
            compatible = member_condition_contains(c,r->value.constraints[p],constraints[p]) &&
                member_condition_contains(c,constraints[p],r->value.constraints[p]);
        if (!compatible) {
            if (c->status == XR_XIR_OK) c->status = XR_XIR_BAD_TYPE;
            return;
        }
    }
    MemberRequirement *requirement = member_alloc(c, 1, sizeof(*requirement));
    if (!requirement) return;
    if (c->requirement_count == UINT32_MAX) { c->status = XR_XIR_BUDGET; return; }
    requirement->next = c->requirements;
    requirement->value = (XrXirInterfaceRequirement){app->index,app->declaration,member,
        signature,method->name,method->receiver,method->own_parameter_count,constraints};
    c->requirements = requirement;
    ++c->requirement_count;
}
static void member_expand_methods(MemberContext *c, const MemberApplication *app) {
    const XrXirInterfaceDeclaration *d = &c->table->declarations[app->declaration];
    for (uint32_t m = 0; m < d->method_count && member_work(c, 1); ++m) {
        const XrXirInterfaceMethod *method = &d->methods[m]; uint32_t own = method->own_parameter_count;
        uint32_t limit = XR_XIR_TYPE_PARAMETER_LIMIT-XR_XIR_TYPE_PARAMETER_BASE;
        if (own > limit-c->ambient_count || own > limit-app->count) { c->status = XR_XIR_BAD_TYPE; return; }
        if (!member_identity(c,c->ambient_count+own)) return;
        MemberApplication substitution = *app; substitution.count += own;
        substitution.arguments = member_alloc(c,substitution.count,sizeof(*substitution.arguments));
        for (uint32_t p = 0; p < substitution.count && member_work(c,1); ++p)
            substitution.arguments[p] = p < app->count ? app->arguments[p] :
                (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+c->ambient_count+p-app->count);
        MemberTypeMap map = {0};
        XrXirType signature = member_resolve(c,&substitution,&map,method->signature);
        XrXirConstraint *constraints = member_alloc(c,own,sizeof(*constraints));
        for (uint32_t p = 0; p < own && member_work(c,1); ++p)
            constraints[p] = member_constraint(c,&substitution,&map,method->constraints[p]);
        if (c->status == XR_XIR_OK) member_requirement(c,app,m,signature,constraints);
    }
}
static void member_expand_parents(MemberContext *c, const MemberApplication *app) {
    const XrXirInterfaceDeclaration *d = &c->table->declarations[app->declaration];
    MemberTypeMap map = {0};
    for (uint32_t p = 0; p < d->parent_count && member_work(c, 1); ++p) {
        const XrXirInterfaceApplication *parent = &d->parents[p];
        XrXirType *arguments = member_alloc(c, parent->argument_count, sizeof(*arguments));
        if (parent->argument_count && !arguments) return;
        for (uint32_t a = 0; a < parent->argument_count && c->status == XR_XIR_OK; ++a)
            arguments[a] = member_resolve(c, app, &map, parent->arguments[a]);
        if (c->status == XR_XIR_OK) member_application(c, parent->declaration, arguments);
    }
}
static void member_roots(MemberContext *c, const XrXirInterfaceApplication *roots, uint32_t count) {
    if (c->base) {
        c->types = *c->base; c->types.interfaces = NULL;
        c->capacity = c->types.count; c->nodes_owned = false;
    }
    for (uint32_t r = 0; r < count && member_work(c,1); ++r) {
        const XrXirInterfaceApplication *root = &roots[r];
        if (!c->table || root->declaration >= c->table->count ||
            (!!root->arguments != !!root->argument_count) ||
            root->argument_count != c->table->declarations[root->declaration].parameter_count) {
            c->status = XR_XIR_BAD_STRUCTURE; return;
        }
        for (uint32_t a = 0; a < root->argument_count && member_work(c,1); ++a) {
            uint32_t span = xr_xir_type_span(c->base,root->arguments[a]);
            if (span > c->ambient_count) { c->status = XR_XIR_BAD_TYPE; return; }
        }
    }
    if (!member_identity(c,c->ambient_count)) return;
    for (uint32_t r = 0; r < count && member_work(c,1); ++r) {
        const XrXirInterfaceApplication *root = &roots[r];
        XrXirType *arguments = member_alloc(c,root->argument_count,sizeof(*arguments));
        if (root->argument_count && !arguments) return;
        if (!member_work(c,(uint64_t)root->argument_count * sizeof(*arguments))) return;
        if (root->argument_count) memcpy(arguments,root->arguments,root->argument_count * sizeof(*arguments));
        member_application(c,root->declaration,arguments);
    }
    for (MemberApplication *app = c->applications; app && member_work(c, 1); app = app->next)
        member_expand_parents(c, app);
}
static XrXirInterfaceClosure *member_publish(MemberContext *c) {
    XrXirInterfaceClosure *closure = member_alloc(c,1,sizeof(*closure));
    if (!closure) return NULL;
    closure->types = c->types;
    closure->applications = member_alloc(c,c->application_count,sizeof(*closure->applications));
    closure->requirements = member_alloc(c,c->requirement_count,sizeof(*closure->requirements));
    if (c->status != XR_XIR_OK) return NULL;
    for (MemberApplication *app = c->applications; app && member_work(c,1); app = app->next)
        closure->applications[app->index] = (XrXirInterfaceApplication){app->declaration,app->arguments,app->count};
    uint32_t index = c->requirement_count;
    for (MemberRequirement *r = c->requirements; r && member_work(c,1); r = r->next)
        closure->requirements[--index] = r->value;
    if (c->status != XR_XIR_OK) return NULL;
    closure->application_count = c->application_count; closure->requirement_count = c->requirement_count;
    closure->memory = c->memory; c->memory = NULL;
    return closure;
}
XR_FUNC XrXirStatus xr_xir_compile_interface_closure_build(const XrXirCompileContext *compile_context, const XrXirInterfaceClosureRoots *request, XrXirInterfaceClosure **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;

    if (!request || !output || !budget || (!!request->roots != !!request->root_count) ||
        (request->table && (!request->table->count || !request->table->declarations))) return XR_XIR_BAD_STRUCTURE;
    MemberContext c = {0}; c.table = request->table; c.source = request->types;
    c.base = request->types; c.remaining = *budget; c.ambient_count = request->ambient_parameter_count;
    if (!member_identity(&c,c.ambient_count)) { member_dispose(&c); return c.status; }
    member_roots(&c,request->roots,request->root_count);
    for (MemberApplication *app = c.applications; app && member_work(&c,1); app = app->next)
        member_expand_methods(&c,app);
    XrXirInterfaceClosure *result = c.status == XR_XIR_OK ? member_publish(&c) : NULL;
    if (c.status == XR_XIR_OK) *output = result;
    if (c.status == XR_XIR_OK) *budget = c.remaining;
    member_dispose(&c); return c.status;
}
XR_FUNC XrXirStatus xr_xir_compile_interface_closure_substitute(const XrXirCompileContext *compile_context, const XrXirInterfaceClosureRequest *request, XrXirInterfaceClosure **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;

    if (!request || !budget || !output || (!!request->roots != !!request->root_count) ||
        (!!request->arguments != !!request->argument_count)) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceTable *table = request->source_types ? request->source_types->interfaces : NULL;
    if (request->root_count && (!table || !table->declarations)) return XR_XIR_BAD_STRUCTURE;
    MemberContext c = {0}; c.table = table; c.source = request->source_types;
    c.base = request->actual_types; c.remaining = *budget;
    c.ambient_count = request->ambient_parameter_count;
    if (!member_identity(&c,c.ambient_count)) { member_dispose(&c); return c.status; }
    if (c.base) { c.types = *c.base; c.capacity = c.types.count; c.nodes_owned = false; }
    MemberApplication substitution = {0};
    substitution.arguments = (XrXirType *)request->arguments; substitution.count = request->argument_count;
    MemberTypeMap map = {0};
    for (uint32_t a = 0; a < request->argument_count && member_work(&c,1); ++a) {
        uint32_t span = xr_xir_type_span(c.base,request->arguments[a]);
        if (span > c.ambient_count) c.status = XR_XIR_BAD_TYPE;
    }
    for (uint32_t r = 0; r < request->root_count && member_work(&c,1); ++r) {
        const XrXirInterfaceApplication *root = &request->roots[r];
        if (root->declaration >= table->count || (!!root->arguments != !!root->argument_count) ||
            root->argument_count != table->declarations[root->declaration].parameter_count) {
            c.status = XR_XIR_BAD_STRUCTURE; break;
        }
        XrXirType *arguments = member_alloc(&c,root->argument_count,sizeof(*arguments));
        for (uint32_t a = 0; a < root->argument_count && member_work(&c,1); ++a)
            arguments[a] = member_resolve(&c,&substitution,&map,root->arguments[a]);
        if (c.status == XR_XIR_OK) member_application(&c,root->declaration,arguments);
    }
    for (MemberApplication *app = c.applications; app && member_work(&c,1); app = app->next)
        member_expand_parents(&c,app);
    for (MemberApplication *app = c.applications; app && member_work(&c,1); app = app->next)
        member_expand_methods(&c,app);
    XrXirInterfaceClosure *result = c.status == XR_XIR_OK ? member_publish(&c) : NULL;
    if (c.status == XR_XIR_OK) *output = result;
    if (c.status == XR_XIR_OK) *budget = c.remaining;
    member_dispose(&c); return c.status;
}
XR_FUNC const XrXirTypes *xr_xir_interface_closure_types(const XrXirInterfaceClosure *closure) {
    return closure ? &closure->types : NULL;
}
XR_FUNC uint32_t xr_xir_interface_closure_application_count(const XrXirInterfaceClosure *closure) {
    return closure ? closure->application_count : 0;
}
XR_FUNC const XrXirInterfaceApplication *xr_xir_interface_closure_application(
    const XrXirInterfaceClosure *closure, uint32_t index) {
    return closure && index < closure->application_count ? &closure->applications[index] : NULL;
}
XR_FUNC uint32_t xr_xir_interface_closure_requirement_count(const XrXirInterfaceClosure *closure) {
    return closure ? closure->requirement_count : 0;
}
XR_FUNC const XrXirInterfaceRequirement *xr_xir_interface_closure_requirement(
    const XrXirInterfaceClosure *closure, uint32_t index) {
    return closure && index < closure->requirement_count ? &closure->requirements[index] : NULL;
}
XR_FUNC void xr_xir_compile_interface_closure_free(XrXirInterfaceClosure *closure) {
    MemberMemory *memory = closure ? closure->memory : NULL;
    while (memory) { MemberMemory *next = memory->next; xr_compile_resources_free(memory); memory = next; }
}
XR_FUNC XrXirStatus xr_xir_compile_interfaces_verify_members_verified(const XrXirCompileContext *compile_context, const XrXirInterfaceTable *table, const XrXirTypes *types) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!budget || (table && (!table->count || !table->declarations))) return XR_XIR_BAD_STRUCTURE;
    if (!table) return XR_XIR_OK;
    XrXirCompileContext remaining = *budget;
    for (uint32_t root = 0; root < table->count; ++root) {
        MemberContext c = {0}; c.table = table; c.source = types; c.base = types; c.remaining = remaining;
        uint32_t count = table->declarations[root].parameter_count;
        c.ambient_count = count;
        XrXirType *arguments = member_alloc(&c,count,sizeof(*arguments));
        for (uint32_t a = 0; a < count && member_work(&c,1); ++a)
            arguments[a] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + a);
        XrXirInterfaceApplication application = {root,arguments,count};
        if (c.status == XR_XIR_OK) member_roots(&c,&application,1);
        for (MemberApplication *app = c.applications; app && member_work(&c,1); app = app->next)
            member_expand_methods(&c,app);
        member_dispose(&c);
        if (c.status != XR_XIR_OK) return c.status;
        /* No closure escapes: all context-owned reservations were released. */

        remaining = c.remaining;
    }
    *budget = remaining; return XR_XIR_OK;
}
