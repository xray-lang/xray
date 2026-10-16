/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_members_input.h - Shared real Source input and owned member facts
 */
#ifndef P09_HASH_MEMBERS_INPUT_H
#define P09_HASH_MEMBERS_INPUT_H
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include <string.h>
static const char members_source[] =
    "fn fetch<K:Hashable,V>(m:Map<K,V>,k:K)->V?{return m.get(k)}\n"
    "export fn entry()->i64{\n"
    "var m=Map<i64,i64>()\nm.set(1,10)\nm.set(2,20)\nconst old=m\nm.set(1,11)\n"
    "if(!m.containsKey(1)||m.containsKey(9)){return -1}\n"
    "if(!m.delete(2)||m.delete(2)){return -2}\n"
    "var s=Set<i64>(#[7])\ns.add(7)\ns.add(7)\ns.add(8)\nconst saved=s\n"
    "if(!s.contains(7)){return -3}\ns.delete(7)\ns.clear()\nm.clear()\n"
    "return len(old)*100+len(m)*10+len(saved)+len(s)\n}\n"
    "export fn present()->i64?{const m=#{\"a\":7}\nreturn m.get(\"a\")}\n"
    "export fn absent()->i64?{const m=#{\"a\":7}\nreturn m.get(\"z\")}\n"
    "export fn generic()->i64?{const m=#{3:9}\nreturn fetch(m,3)}\n"
    "export fn nested()->i64?{var maps=[#{1:2}]\nmaps[0].set(1,3)\nreturn maps[0].get(1)}\n";
static void members_entries(const XrXirModule *module, uint32_t *entries) {
    const char *names[] = {"entry", "present", "absent", "generic", "nested"};
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
static void members_query(const XrXirSourceView *view) {
    CHECK(view && view->complete); uint32_t members = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *record = &view->declarations[i];
        if (record->kind != XR_XIR_SOURCE_MEMBER || !record->parent) continue;
        const XrXirSourceDeclaration *parent = &view->declarations[record->parent - 1];
        if (parent->native_identity != 6 && parent->native_identity != 7) continue;
        ++members; CHECK(record->signature && record->type.known && record->generic_parent == record->parent);
        CHECK(record->generic_parent_count == (parent->native_identity == 6 ? 2u : 1u));
        if (parent->native_identity == 6 && !strcmp(record->name, "get")) {
            const XrXirTypeNode *type = xr_xir_type_node(view->types, record->type.type);
            CHECK(type && type->kind == XR_XIR_TYPE_NULLABLE && type->element == XR_XIR_TYPE_PARAMETER_BASE + 1);
        }
    }
    CHECK(members == 9);
}
#endif // P09_HASH_MEMBERS_INPUT_H
