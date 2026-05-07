/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xa_selection.c - Unit tests for XaSelectionTable
 */

#include "../../../src/frontend/analyzer/xa_selection.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xchecks.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Fake AstNode with just node_id set. */
static AstNode make_node(uint32_t id) {
    AstNode n;
    memset(&n, 0, sizeof(n));
    n.node_id = id;
    return n;
}

static void test_create_free(void) {
    printf("--- test_create_free ---\n");
    XaSelectionTable *t = xa_selection_table_new();
    assert(t != NULL);
    assert(xa_selection_table_size(t) == 0);
    xa_selection_table_free(t);
    printf("  PASS\n");
}

static void test_set_get(void) {
    printf("--- test_set_get ---\n");
    XaSelectionTable *t = xa_selection_table_new();

    AstNode n1 = make_node(100);
    AstNode n2 = make_node(200);

    XaSelection sel1 = {
        .kind = XA_SEL_FIELD,
        .receiver_type = NULL,
        .target_symbol = NULL,
        .field_index = 3,
        .result_type = NULL,
        .is_indirect = false,
        .is_optional = false,
    };
    xa_selection_table_set(t, &n1, &sel1);
    assert(xa_selection_table_size(t) == 1);

    const XaSelection *got = xa_selection_table_get(t, &n1);
    assert(got != NULL);
    assert(got->kind == XA_SEL_FIELD);
    assert(got->field_index == 3);

    /* Lookup of unrecorded node returns NULL */
    assert(xa_selection_table_get(t, &n2) == NULL);

    /* Overwrite existing entry */
    XaSelection sel2 = {
        .kind = XA_SEL_METHOD,
        .receiver_type = NULL,
        .target_symbol = NULL,
        .field_index = -1,
        .result_type = NULL,
        .is_indirect = false,
        .is_optional = true,
    };
    xa_selection_table_set(t, &n1, &sel2);
    assert(xa_selection_table_size(t) == 1);  /* no new entry */
    got = xa_selection_table_get(t, &n1);
    assert(got->kind == XA_SEL_METHOD);
    assert(got->is_optional == true);

    xa_selection_table_free(t);
    printf("  PASS\n");
}

static void test_clear(void) {
    printf("--- test_clear ---\n");
    XaSelectionTable *t = xa_selection_table_new();

    for (uint32_t i = 1; i <= 50; i++) {
        AstNode n = make_node(i);
        XaSelection sel = {
            .kind = XA_SEL_FIELD,
            .field_index = (int32_t)i,
        };
        xa_selection_table_set(t, &n, &sel);
    }
    assert(xa_selection_table_size(t) == 50);

    xa_selection_table_clear(t);
    assert(xa_selection_table_size(t) == 0);

    /* After clear, lookups return NULL */
    AstNode n1 = make_node(1);
    assert(xa_selection_table_get(t, &n1) == NULL);

    xa_selection_table_free(t);
    printf("  PASS\n");
}

static void test_many_entries(void) {
    printf("--- test_many_entries ---\n");
    XaSelectionTable *t = xa_selection_table_new();

    /* Insert enough to trigger multiple rehashes */
    for (uint32_t i = 1; i <= 200; i++) {
        AstNode n = make_node(i);
        XaSelection sel = {
            .kind = (i % 2 == 0) ? XA_SEL_METHOD : XA_SEL_FIELD,
            .field_index = (int32_t)i,
        };
        xa_selection_table_set(t, &n, &sel);
    }
    assert(xa_selection_table_size(t) == 200);

    /* Verify all entries are retrievable */
    for (uint32_t i = 1; i <= 200; i++) {
        AstNode n = make_node(i);
        const XaSelection *got = xa_selection_table_get(t, &n);
        assert(got != NULL);
        assert(got->field_index == (int32_t)i);
    }

    xa_selection_table_free(t);
    printf("  PASS\n");
}

static void test_all_kinds(void) {
    printf("--- test_all_kinds ---\n");
    XaSelectionTable *t = xa_selection_table_new();

    XaSelectionKind kinds[] = {
        XA_SEL_FIELD, XA_SEL_METHOD, XA_SEL_INDEX,
        XA_SEL_STATIC_MEMBER, XA_SEL_MODULE_EXPORT, XA_SEL_ENUM_MEMBER,
    };
    int nkinds = (int)(sizeof(kinds) / sizeof(kinds[0]));

    for (int i = 0; i < nkinds; i++) {
        AstNode n = make_node((uint32_t)(1000 + i));
        XaSelection sel = {
            .kind = kinds[i],
            .field_index = -1,
        };
        xa_selection_table_set(t, &n, &sel);
    }
    assert(xa_selection_table_size(t) == nkinds);

    for (int i = 0; i < nkinds; i++) {
        AstNode n = make_node((uint32_t)(1000 + i));
        const XaSelection *got = xa_selection_table_get(t, &n);
        assert(got != NULL);
        assert(got->kind == kinds[i]);
    }

    xa_selection_table_free(t);
    printf("  PASS\n");
}

int main(void) {
    test_create_free();
    test_set_get();
    test_clear();
    test_many_entries();
    test_all_kinds();
    printf("\n=== All XaSelectionTable tests passed ===\n");
    return 0;
}
