/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_source.h - Source-owned hash type facts and executable carriers
 */
#ifndef P09_HASH_SOURCE_H
#define P09_HASH_SOURCE_H
#include "p09_hash_source_check.h"
static const char hash_source_program[] =
    "export fn entry()->i64{return 42}\n"
    "export fn echo(x:Map<string,Array<i64>>)->Map<string,Array<i64>>{return x}\n"
    "export fn relay(x:Map<string,Array<i64>>)->Map<string,Array<i64>>{return echo(x)}\n"
    "export fn setecho(x:Set<f64>)->Set<f64>{return x}\n"
    "export fn setrelay(x:Set<f64>)->Set<f64>{return setecho(x)}\n";
static void hash_source_query(const XrXirSourceView *view) {
    CHECK(view && view->complete);
    unsigned map = 0, set = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *record = &view->declarations[i];
        if (record->kind != XR_XIR_SOURCE_TYPE || (record->native_identity != 6 && record->native_identity != 7)) continue;
        bool is_map = record->native_identity == 6;
        CHECK(!strcmp(record->name, is_map ? "Map" : "Set") && record->exported);
        CHECK(record->generic_parameter_count == (is_map ? 2u : 1u) && record->generic_constraints);
        CHECK(record->generic_constraints[0].markers == XR_XIR_CONSTRAINT_HASHABLE);
        if (is_map) CHECK(!record->generic_constraints[1].markers);
        CHECK(record->range.module < view->module_count && record->range.line == 2);
        CHECK(!strcmp(view->modules[record->range.module].path, is_map ? "stdlib/types/map.xr" : "stdlib/types/set.xr"));
        uint32_t parameters = 0;
        for (uint32_t p = 0; p < view->declaration_count; ++p) {
            const XrXirSourceDeclaration *parameter = &view->declarations[p];
            if (parameter->parent != record->id || parameter->kind != XR_XIR_SOURCE_TYPE_PARAMETER) continue;
            CHECK(parameters < record->generic_parameter_count && parameter->type.known);
            CHECK(!strcmp(parameter->name, is_map ? (parameters ? "V" : "K") : "T"));
            CHECK(parameter->type.generic_owner == record->id &&
                parameter->type.type == (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + parameters));
            ++parameters;
        }
        CHECK(parameters == record->generic_parameter_count);
        if (is_map) ++map; else ++set;
    }
    CHECK(map == 1 && set == 1);
}
static void hash_source_entries(const XrXirModule *module, uint32_t entries[4]) {
    const char *names[] = {"echo", "relay", "setecho", "setrelay"};
    for (unsigned n = 0; n < 4; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) {
                CHECK(entries[n] == UINT32_MAX); entries[n] = f;
            }
        CHECK(entries[n] != UINT32_MAX);
    }
}
static XrXirArtifact *hash_source_lower(const XrXirCompileContext *context) {
    XrXirSourceResult source = {0};
    CHECK(hash_source_check(context, hash_source_program, &source) == XR_XIR_OK);
    hash_source_query(xr_xir_compile_source_snapshot_view(source.snapshot));
    XrXirArtifact *lowered = hash_source_replay(context, &source);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entries[4]; hash_source_entries(module, entries);
    return lowered;
}
#endif // P09_HASH_SOURCE_H
