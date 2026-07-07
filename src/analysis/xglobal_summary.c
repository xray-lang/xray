/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_summary.c - Whole-program summary/evidence data model
 */

#include "xglobal_summary.h"
#include "../base/xhash.h"
#include "../base/xmalloc.h"
#include "../base/xmemstream.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

XR_FUNC uint32_t xg_name_id(const char *name) {
    if (!name || !name[0])
        return 0;
    uint32_t h = xr_hash_bytes(name, strlen(name));
    return h ? h : 1;
}

static bool reserve_array(void **items, uint32_t *cap, uint32_t needed, size_t elem_size) {
    uint32_t new_cap;
    void *new_items;

    if (!items || !cap || elem_size == 0)
        return false;
    if (*cap >= needed)
        return true;
    new_cap = *cap < 8 ? 8 : *cap;
    while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2)
            return false;
        new_cap *= 2;
    }
    if ((size_t) new_cap > SIZE_MAX / elem_size)
        return false;
    new_items = xr_realloc(*items, (size_t) new_cap * elem_size);
    if (!new_items)
        return false;
    *items = new_items;
    *cap = new_cap;
    return true;
}

static uint64_t hash_mix(uint64_t hash, const void *data, size_t size) {
    uint64_t part = xr_hash_bytes64(data, size);
    hash ^= part + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t hash_u8(uint64_t hash, uint8_t value) {
    return hash_mix(hash, &value, sizeof(value));
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    return hash_mix(hash, &value, sizeof(value));
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    return hash_mix(hash, &value, sizeof(value));
}

static size_t bounded_cstr_len(const char *s, size_t max_len) {
    size_t len = 0;
    if (!s)
        return 0;
    while (len < max_len && s[len])
        len++;
    return len;
}

static uint64_t hash_decl_summary(uint64_t hash, const XgDeclSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->flags);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->signature_key);
    return hash_u32(hash, row->source_span_id);
}

static uint64_t hash_class_summary(uint64_t hash, const XgClassSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u32(hash, row->class_id);
    hash = hash_u32(hash, row->parent_class_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->flags);
    hash = hash_u32(hash, row->field_start);
    hash = hash_u32(hash, row->field_count);
    hash = hash_u32(hash, row->method_start);
    hash = hash_u32(hash, row->method_count);
    hash = hash_u32(hash, row->interface_start);
    hash = hash_u32(hash, row->interface_count);
    return hash_u8(hash, row->decl_kind);
}

static uint64_t hash_method_summary(uint64_t hash, const XgMethodSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->method_id);
    hash = hash_u32(hash, row->owner_class_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->signature_key);
    hash = hash_u32(hash, row->override_of);
    hash = hash_u32(hash, row->default_arg_contract_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_interface_impl_summary(uint64_t hash, const XgInterfaceImplSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->implementor_class_id);
    hash = hash_u32(hash, row->interface_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->source_span_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_body_summary(uint64_t hash, const XgBodySummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->func_id);
    hash = hash_u64(hash, row->body_hash);
    hash = hash_u32(hash, row->effect_bits);
    hash = hash_u32(hash, row->escape_bits);
    hash = hash_u32(hash, row->capability_bits);
    hash = hash_u32(hash, row->callsite_start);
    hash = hash_u32(hash, row->callsite_count);
    hash = hash_u32(hash, row->metadata_use_bits);
    return hash_u32(hash, row->static_data_use_bits);
}

static uint64_t hash_callsite_summary(uint64_t hash, const XgCallsiteSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->callsite_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->static_target_func_id);
    hash = hash_u32(hash, row->receiver_static_class_id);
    hash = hash_u32(hash, row->receiver_static_interface_id);
    hash = hash_u32(hash, row->method_id);
    hash = hash_u32(hash, row->method_name_id);
    hash = hash_u32(hash, row->method_signature_key);
    hash = hash_u32(hash, row->arg_type_key_start);
    hash = hash_u32(hash, row->arg_count);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_link_dependency_summary(uint64_t hash, const XgLinkDependencySummary *row) {
    size_t name_len;
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->link_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->flags);
    name_len = bounded_cstr_len(row->name, sizeof(row->name));
    hash = hash_u32(hash, (uint32_t) name_len);
    return hash_mix(hash, row->name, name_len);
}

static uint64_t hash_build_key(uint64_t hash, const XgBuildKey *key) {
    if (!key)
        return hash_u32(hash, 0);
    hash = hash_u64(hash, key->source_hash);
    hash = hash_u64(hash, key->compiler_semver_hash);
    hash = hash_u64(hash, key->profile_hash);
    hash = hash_u64(hash, key->imported_summary_hash);
    hash = hash_u32(hash, key->module_id);
    return hash_u32(hash, key->profile);
}

XR_FUNC const char *xg_build_profile_name(uint32_t profile) {
    switch ((XgBuildProfile) profile) {
        case XG_BUILD_CHECK:
            return "check";
        case XG_BUILD_DEV:
            return "dev";
        case XG_BUILD_NATIVE_RELEASE:
            return "native_release";
        case XG_BUILD_FREESTANDING:
            return "freestanding";
        case XG_BUILD_DEBUG_TOOLING:
            return "debug_tooling";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_decl_kind_name(uint8_t kind) {
    switch ((XgDeclKind) kind) {
        case XG_DECL_FUNC:
            return "func";
        case XG_DECL_CLASS:
            return "class";
        case XG_DECL_STRUCT:
            return "struct";
        case XG_DECL_UNION:
            return "union";
        case XG_DECL_ENUM:
            return "enum";
        case XG_DECL_INTERFACE:
            return "interface";
        case XG_DECL_GLOBAL:
            return "global";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_callsite_kind_name(uint8_t kind) {
    switch ((XgCallsiteKind) kind) {
        case XG_CALL_DIRECT_FUNC:
            return "direct_func";
        case XG_CALL_METHOD:
            return "method";
        case XG_CALL_INTERFACE:
            return "interface";
        case XG_CALL_CLOSURE:
            return "closure";
        case XG_CALL_NATIVE:
            return "native";
        case XG_CALL_EXTERN:
            return "extern";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_link_dependency_kind_name(uint8_t kind) {
    switch ((XgLinkDependencyKind) kind) {
        case XG_LINK_DEP_EXTERN_DYLIB:
            return "extern_dylib";
        case XG_LINK_DEP_STDLIB_SYMBOL:
            return "stdlib_symbol";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_capability_name(uint32_t capability) {
    switch (capability) {
        case XG_CAP_COROUTINE:
            return "coroutine";
        case XG_CAP_CHANNEL:
            return "channel";
        case XG_CAP_EXCEPTION:
            return "exception";
        case XG_CAP_NATIVE:
            return "native";
        case XG_CAP_EXTERN:
            return "extern";
        case XG_CAP_OBJECTS:
            return "objects";
        case XG_CAP_DEEP_COPY:
            return "deep_copy";
        case XG_CAP_INSTANCEOF:
            return "instanceof";
        case XG_CAP_SYS_THREAD:
            return "sys_thread";
        case XG_CAP_SCOPE:
            return "scope";
        case XG_CAP_TIMER:
            return "timer";
        case XG_CAP_NETPOLL:
            return "netpoll";
        case XG_CAP_TASK:
            return "task";
        case XG_CAP_ATOMIC:
            return "atomic";
        case XG_CAP_WORK_QUEUE:
            return "work_queue";
        case XG_CAP_RESULT_GROUP:
            return "result_group";
        case XG_CAP_COUNTDOWN_LATCH:
            return "countdown_latch";
        case XG_CAP_SEMAPHORE:
            return "semaphore";
        case XG_CAP_EVENT_COUNT:
            return "event_count";
        case XG_CAP_GENERATOR:
            return "generator";
        case XG_CAP_STACKTRACE:
            return "stacktrace";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_capability_catalog(uint32_t *out_count) {
    static const uint32_t capabilities[] = {
        XG_CAP_COROUTINE,    XG_CAP_CHANNEL,         XG_CAP_EXCEPTION,
        XG_CAP_NATIVE,       XG_CAP_EXTERN,          XG_CAP_OBJECTS,
        XG_CAP_DEEP_COPY,    XG_CAP_INSTANCEOF,      XG_CAP_SYS_THREAD,
        XG_CAP_SCOPE,        XG_CAP_TIMER,           XG_CAP_NETPOLL,
        XG_CAP_TASK,         XG_CAP_ATOMIC,          XG_CAP_WORK_QUEUE,
        XG_CAP_RESULT_GROUP, XG_CAP_COUNTDOWN_LATCH, XG_CAP_SEMAPHORE,
        XG_CAP_EVENT_COUNT,  XG_CAP_GENERATOR,       XG_CAP_STACKTRACE,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(capabilities) / sizeof(capabilities[0]));
    return capabilities;
}

XR_FUNC const char *xg_metadata_name(uint32_t metadata) {
    switch (metadata) {
        case XG_METADATA_TYPENAME:
            return "typename";
        case XG_METADATA_DERIVE:
            return "derive";
        case XG_METADATA_DEBUG:
            return "debug";
        case XG_METADATA_TOOLING:
            return "tooling";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_metadata_catalog(uint32_t *out_count) {
    static const uint32_t metadata[] = {
        XG_METADATA_TYPENAME,
        XG_METADATA_DERIVE,
        XG_METADATA_DEBUG,
        XG_METADATA_TOOLING,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(metadata) / sizeof(metadata[0]));
    return metadata;
}

XR_FUNC const char *xg_static_data_name(uint32_t static_data) {
    switch (static_data) {
        case XG_STATIC_DATA_COMPTIME_VALUE:
            return "comptime_value";
        case XG_STATIC_DATA_FIXED_LAYOUT:
            return "fixed_layout";
        case XG_STATIC_DATA_RODATA:
            return "rodata";
        case XG_STATIC_DATA_FREESTANDING_SAFE:
            return "freestanding_safe";
        case XG_STATIC_DATA_RUNTIME_INIT:
            return "runtime_init";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_static_data_catalog(uint32_t *out_count) {
    static const uint32_t static_data[] = {
        XG_STATIC_DATA_COMPTIME_VALUE,    XG_STATIC_DATA_FIXED_LAYOUT, XG_STATIC_DATA_RODATA,
        XG_STATIC_DATA_FREESTANDING_SAFE, XG_STATIC_DATA_RUNTIME_INIT,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(static_data) / sizeof(static_data[0]));
    return static_data;
}

XR_FUNC void xg_global_evidence_init(XgGlobalEvidence *evidence, XgBuildKey key) {
    if (!evidence)
        return;
    memset(evidence, 0, sizeof(*evidence));
    evidence->key = key;
}

XR_FUNC void xg_global_evidence_free(XgGlobalEvidence *evidence) {
    if (!evidence)
        return;
    xr_free(evidence->decls);
    xr_free(evidence->classes);
    xr_free(evidence->methods);
    xr_free(evidence->interface_impls);
    xr_free(evidence->bodies);
    xr_free(evidence->callsites);
    xr_free(evidence->link_deps);
    memset(evidence, 0, sizeof(*evidence));
}

XR_FUNC bool xg_global_evidence_reserve_decls(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->decls, &evidence->decl_cap, capacity,
                                     sizeof(XgDeclSummary));
}

XR_FUNC bool xg_global_evidence_reserve_classes(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->classes, &evidence->class_cap, capacity,
                                     sizeof(XgClassSummary));
}

XR_FUNC bool xg_global_evidence_reserve_methods(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->methods, &evidence->method_cap, capacity,
                                     sizeof(XgMethodSummary));
}

XR_FUNC bool xg_global_evidence_reserve_interface_impls(XgGlobalEvidence *evidence,
                                                        uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->interface_impls, &evidence->interface_impl_cap,
                         capacity, sizeof(XgInterfaceImplSummary));
}

XR_FUNC bool xg_global_evidence_reserve_bodies(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->bodies, &evidence->body_cap, capacity,
                                     sizeof(XgBodySummary));
}

XR_FUNC bool xg_global_evidence_reserve_callsites(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->callsites, &evidence->callsite_cap,
                                     capacity, sizeof(XgCallsiteSummary));
}

XR_FUNC bool xg_global_evidence_reserve_link_deps(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->link_deps, &evidence->link_dep_cap,
                                     capacity, sizeof(XgLinkDependencySummary));
}

XR_FUNC XgDeclSummary *xg_global_evidence_add_decl(XgGlobalEvidence *evidence,
                                                   const XgDeclSummary *summary) {
    XgDeclSummary *row;
    if (!evidence || !summary || !xg_global_evidence_reserve_decls(evidence, evidence->ndecls + 1))
        return NULL;
    row = &evidence->decls[evidence->ndecls++];
    *row = *summary;
    return row;
}

XR_FUNC XgClassSummary *xg_global_evidence_add_class(XgGlobalEvidence *evidence,
                                                     const XgClassSummary *summary) {
    XgClassSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_classes(evidence, evidence->nclasses + 1))
        return NULL;
    row = &evidence->classes[evidence->nclasses++];
    *row = *summary;
    return row;
}

XR_FUNC XgMethodSummary *xg_global_evidence_add_method(XgGlobalEvidence *evidence,
                                                       const XgMethodSummary *summary) {
    XgMethodSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_methods(evidence, evidence->nmethods + 1))
        return NULL;
    row = &evidence->methods[evidence->nmethods++];
    *row = *summary;
    return row;
}

XR_FUNC XgInterfaceImplSummary *
xg_global_evidence_add_interface_impl(XgGlobalEvidence *evidence,
                                      const XgInterfaceImplSummary *summary) {
    XgInterfaceImplSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_interface_impls(evidence, evidence->ninterface_impls + 1))
        return NULL;
    row = &evidence->interface_impls[evidence->ninterface_impls++];
    *row = *summary;
    return row;
}

XR_FUNC XgBodySummary *xg_global_evidence_add_body(XgGlobalEvidence *evidence,
                                                   const XgBodySummary *summary) {
    XgBodySummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_bodies(evidence, evidence->nbodies + 1))
        return NULL;
    row = &evidence->bodies[evidence->nbodies++];
    *row = *summary;
    return row;
}

XR_FUNC XgCallsiteSummary *xg_global_evidence_add_callsite(XgGlobalEvidence *evidence,
                                                           const XgCallsiteSummary *summary) {
    XgCallsiteSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_callsites(evidence, evidence->ncallsites + 1))
        return NULL;
    row = &evidence->callsites[evidence->ncallsites++];
    *row = *summary;
    return row;
}

XR_FUNC XgLinkDependencySummary *
xg_global_evidence_add_link_dependency(XgGlobalEvidence *evidence,
                                       const XgLinkDependencySummary *summary) {
    XgLinkDependencySummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_link_deps(evidence, evidence->nlink_deps + 1))
        return NULL;
    row = &evidence->link_deps[evidence->nlink_deps++];
    *row = *summary;
    row->name[XG_LINK_DEP_NAME_MAX - 1] = '\0';
    return row;
}

XR_FUNC const XgCallsiteSummary *xg_global_evidence_find_callsite(const XgGlobalEvidence *evidence,
                                                                  XgCallsiteId callsite_id) {
    if (!evidence || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        if (evidence->callsites[i].callsite_id == callsite_id)
            return &evidence->callsites[i];
    }
    return NULL;
}

XR_FUNC uint64_t xg_global_evidence_hash(const XgGlobalEvidence *evidence) {
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    if (!evidence)
        return hash;
    hash = hash_build_key(hash, &evidence->key);
    hash = hash_mix(hash, &evidence->ndecls, sizeof(evidence->ndecls));
    hash = hash_mix(hash, &evidence->nclasses, sizeof(evidence->nclasses));
    hash = hash_mix(hash, &evidence->nmethods, sizeof(evidence->nmethods));
    hash = hash_mix(hash, &evidence->ninterface_impls, sizeof(evidence->ninterface_impls));
    hash = hash_mix(hash, &evidence->nbodies, sizeof(evidence->nbodies));
    hash = hash_mix(hash, &evidence->ncallsites, sizeof(evidence->ncallsites));
    hash = hash_mix(hash, &evidence->nlink_deps, sizeof(evidence->nlink_deps));
    for (uint32_t i = 0; i < evidence->ndecls; i++)
        hash = hash_decl_summary(hash, &evidence->decls[i]);
    for (uint32_t i = 0; i < evidence->nclasses; i++)
        hash = hash_class_summary(hash, &evidence->classes[i]);
    for (uint32_t i = 0; i < evidence->nmethods; i++)
        hash = hash_method_summary(hash, &evidence->methods[i]);
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++)
        hash = hash_interface_impl_summary(hash, &evidence->interface_impls[i]);
    for (uint32_t i = 0; i < evidence->nbodies; i++)
        hash = hash_body_summary(hash, &evidence->bodies[i]);
    for (uint32_t i = 0; i < evidence->ncallsites; i++)
        hash = hash_callsite_summary(hash, &evidence->callsites[i]);
    for (uint32_t i = 0; i < evidence->nlink_deps; i++)
        hash = hash_link_dependency_summary(hash, &evidence->link_deps[i]);
    return hash == 0 ? 1 : hash;
}

XR_FUNC char *xg_global_evidence_dump(const XgGlobalEvidence *evidence) {
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *out;

    if (!evidence)
        return NULL;

    out = xr_open_memstream(&buf, &bufsz);
    if (!out)
        return NULL;

    fprintf(out, "xglobal-evidence v0 profile=%s hash=%016" PRIx64 "\n",
            xg_build_profile_name(evidence->key.profile), xg_global_evidence_hash(evidence));
    fprintf(out,
            "build-key module=%u source=%016" PRIx64 " compiler=%016" PRIx64 " profile=%016" PRIx64
            " imports=%016" PRIx64 "\n",
            evidence->key.module_id, evidence->key.source_hash, evidence->key.compiler_semver_hash,
            evidence->key.profile_hash, evidence->key.imported_summary_hash);
    fprintf(out,
            "counts decls=%u classes=%u methods=%u interface_impls=%u bodies=%u callsites=%u "
            "link_deps=%u\n",
            evidence->ndecls, evidence->nclasses, evidence->nmethods, evidence->ninterface_impls,
            evidence->nbodies, evidence->ncallsites, evidence->nlink_deps);

    for (uint32_t i = 0; i < evidence->ndecls; i++) {
        const XgDeclSummary *d = &evidence->decls[i];
        fprintf(out, "decl %u id=%u module=%u kind=%s flags=0x%x name=%u type=%u sig=%u span=%u\n",
                i, d->decl_id, d->module_id, xg_decl_kind_name(d->kind), d->flags, d->name_id,
                d->type_key, d->signature_key, d->source_span_id);
    }
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        const XgClassSummary *c = &evidence->classes[i];
        fprintf(out,
                "class %u id=%u module=%u decl=%u name=%u parent=%u kind=%s flags=0x%x "
                "fields=%u+%u methods=%u+%u interfaces=%u+%u\n",
                i, c->class_id, c->module_id, c->decl_id, c->name_id, c->parent_class_id,
                xg_decl_kind_name(c->decl_kind ? c->decl_kind : XG_DECL_CLASS), c->flags,
                c->field_start, c->field_count, c->method_start, c->method_count,
                c->interface_start, c->interface_count);
    }
    for (uint32_t i = 0; i < evidence->nmethods; i++) {
        const XgMethodSummary *m = &evidence->methods[i];
        fprintf(out,
                "method %u id=%u owner=%u name=%u sig=%u override_of=%u defaults=%u flags=0x%x\n",
                i, m->method_id, m->owner_class_id, m->name_id, m->signature_key, m->override_of,
                m->default_arg_contract_id, m->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
        fprintf(out, "interface-impl %u class=%u interface=%u name=%u type=%u span=%u flags=0x%x\n",
                i, impl->implementor_class_id, impl->interface_id, impl->name_id, impl->type_key,
                impl->source_span_id, impl->flags);
    }
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *b = &evidence->bodies[i];
        fprintf(out,
                "body %u func=%u hash=%016" PRIx64
                " effect=0x%x escape=0x%x caps=0x%x callsites=%u+%u metadata=0x%x static=0x%x\n",
                i, b->func_id, b->body_hash, b->effect_bits, b->escape_bits, b->capability_bits,
                b->callsite_start, b->callsite_count, b->metadata_use_bits,
                b->static_data_use_bits);
    }
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *c = &evidence->callsites[i];
        fprintf(out,
                "callsite %u id=%u owner=%u span=%u kind=%s target=%u recv_class=%u "
                "recv_iface=%u method=%u method_name=%u method_sig=%u args=%u+%u flags=0x%x\n",
                i, c->callsite_id, c->owner_func_id, c->source_span_id,
                xg_callsite_kind_name(c->kind), c->static_target_func_id,
                c->receiver_static_class_id, c->receiver_static_interface_id, c->method_id,
                c->method_name_id, c->method_signature_key, c->arg_type_key_start,
                (unsigned) c->arg_count, c->flags);
    }
    for (uint32_t i = 0; i < evidence->nlink_deps; i++) {
        const XgLinkDependencySummary *dep = &evidence->link_deps[i];
        fprintf(out,
                "link-dep %u id=%u module=%u decl=%u span=%u kind=%s name_id=%u name=%s "
                "flags=0x%x\n",
                i, dep->link_id, dep->module_id, dep->decl_id, dep->source_span_id,
                xg_link_dependency_kind_name(dep->kind), dep->name_id, dep->name, dep->flags);
    }

    if (ferror(out)) {
        (void) xr_close_memstream(out, &buf, &bufsz);
        xr_free(buf);
        return NULL;
    }
    if (xr_close_memstream(out, &buf, &bufsz) != 0)
        return NULL;
    return buf;
}
