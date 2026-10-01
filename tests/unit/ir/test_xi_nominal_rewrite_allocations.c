/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_nominal_rewrite_allocations.c - Atomic receiving Xi type ownership
 */

#include "ir/xi.h"
#include "ir/xi_core_api.h"
#include "ir/xi_module.h"
#include "ir/xi_semantic_snapshot.h"
#include "runtime/class/xclass_info.h"
#include "runtime/value/xtype.h"
#include "../program/xr_program_allocation_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #c); abort(); } } while (0)

static size_t attempts, fail_at, live_count;
static void *live[4096];

static void *track(void *pointer) {
    REQUIRE(pointer != NULL);
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i])
            continue;
        live[i] = pointer;
        live_count++;
        return pointer;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_program_test_system_calloc(count, size));
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at)
        return NULL;
    if (!pointer)
        return track(xr_program_test_system_realloc(NULL, size));
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i] != pointer)
            continue;
        void *grown = xr_program_test_system_realloc(pointer, size);
        REQUIRE(grown != NULL);
        live[i] = grown;
        return grown;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0; i < XR_COUNTOF(live); i++) {
        if (live[i] != pointer)
            continue;
        live[i] = NULL;
        live_count--;
        xr_program_test_system_free(pointer);
        return;
    }
    abort();
}

typedef struct TypeRewriteFixture {
    XiFunc *root;
    XiValue *value;
    XiValue *captured;
    XiModule module;
    XiModuleExport exported;
    XiCapture capture;
    XiClassData klass;
    XrClassInfo info;
    XrType source;
    XrType canonical;
    XrType array;
    XrType tuple;
    XrType callable;
    XrFunctionParam parameter;
    XrType *tuple_items[2];
    XrType *field;
    XrType *source_variable;
    XiModuleSlot slot;
} TypeRewriteFixture;

static void fixture_init(TypeRewriteFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->info = (XrClassInfo) {.name = "Grounded$scalar", .xg_class_id = 207};
    fixture->source = (XrType) {.kind = XR_KIND_INSTANCE, .semantic_type_id = 501,
        .instance = {.class_ref = &fixture->info, .class_name = "Grounded<T>"}};
    fixture->canonical = (XrType) {.kind = XR_KIND_INSTANCE,
        .instance = {.class_ref = &fixture->info, .class_name = "Grounded$scalar"}};
    fixture->array = (XrType) {.kind = XR_KIND_ARRAY,
        .container = {.element_type = &fixture->source}};
    fixture->tuple_items[0] = &fixture->source;
    fixture->tuple_items[1] = &fixture->array;
    fixture->tuple = (XrType) {.kind = XR_KIND_TUPLE,
        .tuple = {.element_count = 2, .element_types = fixture->tuple_items}};
    fixture->parameter = (XrFunctionParam) {.type = &fixture->tuple, .mode = XR_PARAM_READ};
    fixture->callable = (XrType) {.kind = XR_KIND_FUNCTION,
        .function = {.params = &fixture->parameter, .param_count = 1, .min_params = 1,
            .return_type = &fixture->source, .receiver_mode = XR_PARAM_READ,
            .throw_effect = XR_FN_EFFECT_POLY}};
    fixture->root = xi_func_new("rewrite", &fixture->tuple);
    REQUIRE(fixture->root != NULL);
    XiBlock *block = xi_block_new(fixture->root);
    REQUIRE(block != NULL);
    fixture->value = xi_value_new(fixture->root, block, XI_CONST, &fixture->source, 0);
    fixture->captured = xi_value_new(fixture->root, block, XI_CONST, &fixture->callable, 0);
    REQUIRE(fixture->value && fixture->captured);
    xi_block_set_return(block, fixture->value);
    fixture->capture.type = &fixture->array;
    fixture->root->captures[0] = fixture->capture;
    fixture->root->ncaptures = 1;
    fixture->root->source_callable_type = &fixture->callable;
    fixture->source_variable = &fixture->array;
    fixture->root->source_var_count = 1;
    fixture->root->source_var_types = &fixture->source_variable;
    fixture->slot.type = &fixture->tuple;
    fixture->root->module_slots = &fixture->slot;
    fixture->root->nshared = 1;
    fixture->exported.value_type = &fixture->callable;
    fixture->module = (XiModule) {.init = fixture->root, .exports = &fixture->exported, .nexports = 1};
    fixture->field = &fixture->source;
    fixture->klass = (XiClassData) {.instance_field_count = 1, .instance_field_types = &fixture->field};
    fixture->module.classes = (XiClassData **) xi_func_arena_alloc(fixture->root, sizeof(XiClassData *));
    REQUIRE(fixture->module.classes != NULL);
    fixture->module.classes[0] = &fixture->klass;
    fixture->module.nclasses = 1;
    fixture->root->module = &fixture->module;
    /* Force subsequent owned snapshot construction across real arena growth. */
    REQUIRE(xi_func_arena_alloc(fixture->root, XI_ARENA_INITIAL_SIZE) != NULL);
}

static void fixture_destroy(TypeRewriteFixture *fixture) {
    fixture->root->ncaptures = 0;
    fixture->root->module = NULL;
    xi_func_free(fixture->root);
    REQUIRE(live_count == 0);
}

static void require_unchanged(const TypeRewriteFixture *fixture) {
    REQUIRE(fixture->root->return_type == &fixture->tuple);
    REQUIRE(fixture->value->type == &fixture->source);
    REQUIRE(fixture->captured->type == &fixture->callable);
    REQUIRE(fixture->root->source_callable_type == &fixture->callable);
    REQUIRE(fixture->root->captures[0].type == &fixture->array && fixture->source_variable == &fixture->array);
    REQUIRE(fixture->root->module_slots[0].type == &fixture->tuple && fixture->exported.value_type == &fixture->callable);
    REQUIRE(fixture->field == &fixture->source);
    REQUIRE(fixture->array.container.element_type == &fixture->source);
    REQUIRE(fixture->tuple_items[0] == &fixture->source);
    REQUIRE(fixture->parameter.type == &fixture->tuple);
}

static void require_owned(const TypeRewriteFixture *fixture) {
    XiFunc *root = fixture->root;
    XrType *nominal = fixture->value->type;
    REQUIRE(nominal != &fixture->canonical && nominal != &fixture->source);
    REQUIRE(xi_func_arena_contains(root, nominal, sizeof(*nominal)));
    REQUIRE(nominal->semantic_type_id == 0 && nominal->instance.class_ref->xg_class_id == 207);
    REQUIRE(strcmp(nominal->instance.class_name, "Grounded$scalar") == 0);
    REQUIRE(root->return_type != &fixture->tuple);
    REQUIRE(root->return_type->tuple.element_types[0] == nominal);
    REQUIRE(root->return_type->tuple.element_types[1]->container.element_type == nominal);
    REQUIRE(root->source_callable_type == fixture->captured->type);
    REQUIRE(root->source_callable_type->function.return_type == nominal);
    REQUIRE(root->source_callable_type->function.params[0].type == root->return_type);
    REQUIRE(root->captures[0].type->container.element_type == nominal);
    REQUIRE(fixture->source_variable == root->captures[0].type);
    REQUIRE(root->module_slots[0].type == root->return_type);
    REQUIRE(fixture->exported.value_type == root->source_callable_type);
    REQUIRE(fixture->field == nominal);
    REQUIRE(fixture->source.semantic_type_id == 501);
    REQUIRE(fixture->parameter.type == &fixture->tuple);
}

int main(void) {
    TypeRewriteFixture fixture;
    fixture_init(&fixture);
    attempts = fail_at = 0;
    XiSemanticTypeReplacement replacement = {&fixture.source, &fixture.canonical};
    REQUIRE(xi_semantic_type_rewrite_atomic(fixture.root, &replacement, 1));
    size_t total = attempts;
    REQUIRE(total > 0);
    require_owned(&fixture);
    fixture_destroy(&fixture);
    for (size_t nth = 1; nth <= total; nth++) {
        fixture_init(&fixture);
        replacement = (XiSemanticTypeReplacement) {&fixture.source, &fixture.canonical};
        attempts = 0;
        fail_at = nth;
        REQUIRE(!xi_semantic_type_rewrite_atomic(fixture.root, &replacement, 1));
        REQUIRE(attempts >= nth);
        require_unchanged(&fixture);
        fail_at = 0;
        fixture_destroy(&fixture);
    }
    printf("Xi owned type graph: %zu real allocator failures, no partial publication, live=0\n", total);
    return 0;
}
