/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_projection_input.h - Ordered owned snapshots across Source stages
 */
#ifndef P09_HASH_PROJECTION_INPUT_H
#define P09_HASH_PROJECTION_INPUT_H
#include "shared/xnative_declaration.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
static const char projection_source[] =
    "fn project<K:Hashable,V>(m:Map<K,V>)->Array<(K,V)>{return m.entries()}\n"
    "export fn entry()->i64{var m=#{3:30,1:10,2:20}\n"
    "m.set(1,11)\nm.delete(3)\nm.set(3,33)\nconst k=m.keys()\nconst v=m.values()\n"
    "var copy=k\ncopy[0]=8\nm.clear()\n"
    "var s=#[7,8,7]\nconst sv=s.values()\ns.delete(7)\ns.add(7)\nconst reordered=s.values()\ns.clear()\n"
    "if(len(m.keys())!=0||len(m.values())!=0||len(m.entries())!=0||len(s.values())!=0){return -1}\n"
    "if(k[0]!=1||k[1]!=2||k[2]!=3||copy[0]!=8){return -2}\n"
    "if(v[0]!=11||v[1]!=20||v[2]!=33||sv[0]!=7||sv[1]!=8){return -3}\n"
    "if(reordered[0]!=8||reordered[1]!=7){return -4}\nreturn 123}\n"
    "export fn keys_late()->Array<string>{var m=#{\"a\":1,\"b\":2}\nconst a=m.keys()\nm.clear()\nreturn a}\n"
    "export fn values_late()->Array<string>{var m=#{1:\"alive\",2:\"tail\"}\nconst a=m.values()\nm.set(1,\"changed\")\nm.clear()\nreturn a}\n"
    "export fn generic()->Array<(i64,string)>{var m=#{2:\"two\",1:\"one\"}\nconst a=project(m)\nm.clear()\nreturn a}\n"
    "export fn nested()->Array<i64>{var maps=[#{5:7,3:9}]\nconst a=maps[0].keys()\nmaps[0].clear()\nreturn a}\n";
static void projection_entries(const XrXirModule *module, uint32_t *entries) {
    const char *names[] = {"entry", "keys_late", "values_late", "generic", "nested"};
    for (uint32_t n = 0; n < 5; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) {
                CHECK(entries[n] == UINT32_MAX); entries[n] = f;
            }
        CHECK(entries[n] != UINT32_MAX);
    }
}
static void projection_query(const XrXirSourceView *view) {
    CHECK(view && view->complete); uint32_t projections = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *record = &view->declarations[i];
        if (record->kind != XR_XIR_SOURCE_MEMBER || !record->parent) continue;
        const XrXirSourceDeclaration *parent = &view->declarations[record->parent - 1];
        bool map = parent->native_identity == XR_NATIVE_DECLARATION_MAP;
        if (!map && parent->native_identity != XR_NATIVE_DECLARATION_SET) continue;
        if (strcmp(record->name, "keys") && strcmp(record->name, "values") && strcmp(record->name, "entries")) continue;
        ++projections;
        CHECK(record->type.known && record->type.generic_owner == record->parent);
        CHECK(xr_xir_type_is_array(view->types, record->type.type));
        XrXirType element = xr_xir_array_element(view->types, record->type.type);
        if (!strcmp(record->name, "entries")) {
            const XrXirTypeNode *tuple = xr_xir_tuple_signature(view->types, element);
            CHECK(map && tuple && tuple->parameter_count == 2 &&
                tuple->parameters[0].type == XR_XIR_TYPE_PARAMETER_BASE &&
                tuple->parameters[1].type == XR_XIR_TYPE_PARAMETER_BASE + 1);
        } else CHECK(element == (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + (map && !strcmp(record->name, "values"))));
    }
    CHECK(projections == 4);
}
#endif // P09_HASH_PROJECTION_INPUT_H
