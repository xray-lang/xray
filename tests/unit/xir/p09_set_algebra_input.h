/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_set_algebra_input.h - Real ordered binary Set queries and generic ownership
 */
#ifndef P09_SET_ALGEBRA_INPUT_H
#define P09_SET_ALGEBRA_INPUT_H
#include "shared/xnative_declaration.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_map.h"
static const char algebra_source[] =
    "fn merge<T:Hashable>(left:Set<T>,right:Set<T>)->Set<T>{return left.union(right)}\n"
    "fn order(a:Set<i64>,u:Set<i64>,i:Set<i64>,d:Set<i64>,x:Set<i64>)->i64{\n"
    "const ua=u.toArray()\n"
    "const ia=i.toArray()\n"
    "const da=d.toArray()\n"
    "const xa=x.toArray()\n"
    "if(len(ua)!=5){return -1}\n"
    "if(ua[0]!=3){return -1}\n"
    "if(ua[1]!=1){return -1}\n"
    "if(ua[2]!=2){return -1}\n"
    "if(ua[3]!=4){return -1}\n"
    "if(ua[4]!=5){return -1}\n"
    "if(len(ia)!=2){return -2}\n"
    "if(ia[0]!=1){return -2}\n"
    "if(ia[1]!=2){return -2}\n"
    "if(len(da)!=1){return -2}\n"
    "if(da[0]!=3){return -2}\n"
    "if(len(xa)!=3){return -3}\n"
    "if(xa[0]!=3){return -3}\n"
    "if(xa[1]!=4){return -3}\n"
    "if(xa[2]!=5){return -3}\n"
    "\n"
    "const e=Set<i64>()\n"
    "if(!e.isSubset(a)){return -4}\n"
    "if(a.isSubset(e)){return -4}\n"
    "if(!a.isSubset(a)){return -4}\n"
    "if(!a.isSuperset(i)){return -4}\n"
    "if(i.isSuperset(a)){return -4}\n"
    "if(len(a.difference(a))!=0){return -5}\n"
    "if(len(a.symmetricDifference(a))!=0){return -5}\n"
    "if(len(e.union(e))!=0){return -5}\n"
    "return 0}\n"
    "fn evaluation()->i64{\n"
    "var trace=0\n"
    "var s=#[1]\n"
    "fn receiver()->Set<i64>{trace=trace*10+1\n"
    "return s}\n"
    "fn other()->Set<i64>{trace=trace*10+2\n"
    "s=#[9]\n"
    "return #[2]}\n"
    "const ordered=receiver().union(other())\n"
    "if(trace!=12){return -7}\n"
    "if(!ordered.contains(1)){return -7}\n"
    "if(!ordered.contains(2)){return -7}\n"
    "if(ordered.contains(9)){return -7}\n"
    "s=#[1]\n"
    "const snap=s.union(other())\n"
    "if(!snap.contains(1)){return -8}\n"
    "if(snap.contains(9)){return -8}\n"
    "return 0}\n"
    "export fn entry()->i64{var a=#[3,1,2]\n"
    "var b=#[2,4,1,5]\n"
    "const old=a\n"
    "var u=a.union(b)\n"
    "const i=a.intersection(b)\n"
    "const d=a.difference(b)\n"
    "const x=a.symmetricDifference(b)\n"
    "const checked=order(a,u,i,d,x)\n"
    "if(checked!=0){return checked}\n"
    "const saved=u\n"
    "u.delete(3)\n"
    "a.clear()\n"
    "b.add(9)\n"
    "if(!saved.contains(3)){return -6}\n"
    "if(u.contains(3)){return -6}\n"
    "if(len(old)!=3){return -6}\n"
    "if(len(a)!=0){return -6}\n"
    "if(saved.contains(9)){return -6}\n"
    "const evaluated=evaluation()\n"
    "if(evaluated!=0){return evaluated}\n"
    "return 123}\n"
    "export fn late_union()->Set<string>{var a=#[\"a\",\"b\"]\n"
    "var b=#[\"b\",\"c\"]\n"
    "const u=a.union(b)\n"
    "const v=u.toArray()\n"
    "if(v[0]!=\"a\"){return a}\n"
    "a.clear()\n"
    "b.clear()\n"
    "return u}\n"
    "export fn late_intersection()->Set<string>{var a=#[\"a\",\"b\"]\n"
    "var b=#[\"b\",\"c\"]\n"
    "const i=a.intersection(b)\n"
    "a.clear()\n"
    "b.clear()\n"
    "return i}\n"
    "export fn generic()->Set<i64>{var a=#[3,1]\n"
    "const u=merge(a,#[1,2])\n"
    "a.clear()\n"
    "return u}\n"
    "export fn nested()->Set<i64>{var sets=[#[9,4,1]]\n"
    "const u=sets[0].union(#[4,2])\n"
    "sets[0].clear()\n"
    "return u}\n";
static void algebra_entries(const XrXirModule *module, uint32_t *entries) {
    const char *names[] = {"entry", "late_union", "late_intersection", "generic", "nested"};
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
static void algebra_query(const XrXirSourceView *view) {
    CHECK(view && view->complete); uint32_t binary = 0, array = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *record = &view->declarations[i];
        if (record->kind != XR_XIR_SOURCE_MEMBER || !record->parent ||
            view->declarations[record->parent - 1].native_identity != XR_NATIVE_DECLARATION_SET) continue;
        bool relation = !strcmp(record->name, "isSubset") || !strcmp(record->name, "isSuperset");
        bool operation = relation || !strcmp(record->name, "union") || !strcmp(record->name, "intersection") ||
            !strcmp(record->name, "difference") || !strcmp(record->name, "symmetricDifference");
        if (operation) {
            ++binary; CHECK(record->type.known && record->parameter_count == 1 && record->parameters[0].known);
            CHECK(record->parameters[0].generic_owner == record->parent);
            CHECK(xr_xir_type_is_set(view->types, record->parameters[0].type) &&
                xr_xir_set_element(view->types, record->parameters[0].type) == XR_XIR_TYPE_PARAMETER_BASE);
            CHECK(relation ? record->type.type == XR_XIR_BOOL :
                record->type.type == record->parameters[0].type && record->type.generic_owner == record->parent);
        } else if (!strcmp(record->name, "toArray")) {
            ++array; CHECK(record->type.known && !record->parameter_count &&
                xr_xir_array_element(view->types, record->type.type) == XR_XIR_TYPE_PARAMETER_BASE);
        }
    }
    CHECK(binary == 6 && array == 1);
}
#endif // P09_SET_ALGEBRA_INPUT_H
