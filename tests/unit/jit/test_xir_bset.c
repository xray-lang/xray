/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_bset.c - Unit tests for XIR bitset data structure
 */

#include <stdio.h>
#include <assert.h>
#include "../../../src/jit/xir_bset.h"
#include "../test_win_compat.h"

static void test_init_zero(void) {
    fprintf(stderr, "  test_init_zero...");
    XirBSet bs;
    xir_bset_init(&bs, 128);
    assert(bs.nw == 2);
    assert(xir_bset_empty(&bs));
    assert(xir_bset_count(&bs) == 0);
    xir_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_set_has_clr(void) {
    fprintf(stderr, "  test_set_has_clr...");
    XirBSet bs;
    xir_bset_init(&bs, 256);

    assert(!xir_bset_has(&bs, 0));
    assert(!xir_bset_has(&bs, 63));
    assert(!xir_bset_has(&bs, 64));
    assert(!xir_bset_has(&bs, 255));

    xir_bset_set(&bs, 0);
    xir_bset_set(&bs, 63);
    xir_bset_set(&bs, 64);
    xir_bset_set(&bs, 255);

    assert(xir_bset_has(&bs, 0));
    assert(xir_bset_has(&bs, 63));
    assert(xir_bset_has(&bs, 64));
    assert(xir_bset_has(&bs, 255));
    assert(!xir_bset_has(&bs, 1));
    assert(!xir_bset_has(&bs, 128));

    assert(xir_bset_count(&bs) == 4);

    xir_bset_clr(&bs, 63);
    assert(!xir_bset_has(&bs, 63));
    assert(xir_bset_count(&bs) == 3);

    xir_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_union(void) {
    fprintf(stderr, "  test_union...");
    XirBSet a, b;
    xir_bset_init(&a, 128);
    xir_bset_init(&b, 128);

    xir_bset_set(&a, 0);
    xir_bset_set(&a, 10);
    xir_bset_set(&b, 10);
    xir_bset_set(&b, 100);

    xir_bset_union(&a, &b);
    assert(xir_bset_has(&a, 0));
    assert(xir_bset_has(&a, 10));
    assert(xir_bset_has(&a, 100));
    assert(xir_bset_count(&a) == 3);

    xir_bset_free(&a);
    xir_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_inter(void) {
    fprintf(stderr, "  test_inter...");
    XirBSet a, b;
    xir_bset_init(&a, 128);
    xir_bset_init(&b, 128);

    xir_bset_set(&a, 0);
    xir_bset_set(&a, 10);
    xir_bset_set(&a, 50);
    xir_bset_set(&b, 10);
    xir_bset_set(&b, 50);
    xir_bset_set(&b, 100);

    xir_bset_inter(&a, &b);
    assert(!xir_bset_has(&a, 0));
    assert(xir_bset_has(&a, 10));
    assert(xir_bset_has(&a, 50));
    assert(!xir_bset_has(&a, 100));
    assert(xir_bset_count(&a) == 2);

    xir_bset_free(&a);
    xir_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_diff(void) {
    fprintf(stderr, "  test_diff...");
    XirBSet a, b;
    xir_bset_init(&a, 128);
    xir_bset_init(&b, 128);

    xir_bset_set(&a, 0);
    xir_bset_set(&a, 10);
    xir_bset_set(&a, 50);
    xir_bset_set(&b, 10);

    xir_bset_diff(&a, &b);
    assert(xir_bset_has(&a, 0));
    assert(!xir_bset_has(&a, 10));
    assert(xir_bset_has(&a, 50));
    assert(xir_bset_count(&a) == 2);

    xir_bset_free(&a);
    xir_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_copy(void) {
    fprintf(stderr, "  test_copy...");
    XirBSet a, b;
    xir_bset_init(&a, 128);
    xir_bset_init(&b, 128);

    xir_bset_set(&a, 5);
    xir_bset_set(&a, 77);
    xir_bset_copy(&b, &a);
    assert(xir_bset_has(&b, 5));
    assert(xir_bset_has(&b, 77));
    assert(xir_bset_equal(&a, &b));

    xir_bset_free(&a);
    xir_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_equal(void) {
    fprintf(stderr, "  test_equal...");
    XirBSet a, b;
    xir_bset_init(&a, 128);
    xir_bset_init(&b, 128);

    assert(xir_bset_equal(&a, &b));

    xir_bset_set(&a, 42);
    assert(!xir_bset_equal(&a, &b));

    xir_bset_set(&b, 42);
    assert(xir_bset_equal(&a, &b));

    xir_bset_free(&a);
    xir_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_iter(void) {
    fprintf(stderr, "  test_iter...");
    XirBSet bs;
    xir_bset_init(&bs, 256);

    xir_bset_set(&bs, 3);
    xir_bset_set(&bs, 64);
    xir_bset_set(&bs, 130);
    xir_bset_set(&bs, 200);

    int iter = 0;
    int bit;
    int collected[4];
    int n = 0;
    while ((bit = xir_bset_iter(&bs, &iter)) >= 0) {
        assert(n < 4);
        collected[n++] = bit;
    }
    assert(n == 4);
    assert(collected[0] == 3);
    assert(collected[1] == 64);
    assert(collected[2] == 130);
    assert(collected[3] == 200);

    xir_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_iter_empty(void) {
    fprintf(stderr, "  test_iter_empty...");
    XirBSet bs;
    xir_bset_init(&bs, 128);

    int iter = 0;
    assert(xir_bset_iter(&bs, &iter) == -1);

    xir_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_fill(void) {
    fprintf(stderr, "  test_fill...");
    XirBSet bs;
    xir_bset_init(&bs, 128);

    xir_bset_fill(&bs);
    assert(xir_bset_has(&bs, 0));
    assert(xir_bset_has(&bs, 63));
    assert(xir_bset_has(&bs, 64));
    assert(xir_bset_has(&bs, 127));
    assert(xir_bset_count(&bs) == 128);

    xir_bset_zero(&bs);
    assert(xir_bset_empty(&bs));

    xir_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_large(void) {
    fprintf(stderr, "  test_large...");
    XirBSet bs;
    xir_bset_init(&bs, 4096);

    xir_bset_set(&bs, 0);
    xir_bset_set(&bs, 4095);
    xir_bset_set(&bs, 2048);
    assert(xir_bset_count(&bs) == 3);
    assert(xir_bset_has(&bs, 4095));

    xir_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xir_bset ===\n");

    test_init_zero();
    test_set_has_clr();
    test_union();
    test_inter();
    test_diff();
    test_copy();
    test_equal();
    test_iter();
    test_iter_empty();
    test_fill();
    test_large();

    fprintf(stderr, "All 11 tests passed!\n");
    return 0;
}
