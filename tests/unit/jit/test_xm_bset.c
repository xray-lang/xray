/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_bset.c - Unit tests for Xm bitset data structure
 */

#include <stdio.h>
#include <assert.h>
#include "../../../src/jit/xm_bset.h"
#include "../test_win_compat.h"

static void test_init_zero(void) {
    fprintf(stderr, "  test_init_zero...");
    XmBSet bs;
    xm_bset_init(&bs, 128);
    assert(bs.nw == 2);
    assert(xm_bset_empty(&bs));
    assert(xm_bset_count(&bs) == 0);
    xm_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_set_has_clr(void) {
    fprintf(stderr, "  test_set_has_clr...");
    XmBSet bs;
    xm_bset_init(&bs, 256);

    assert(!xm_bset_has(&bs, 0));
    assert(!xm_bset_has(&bs, 63));
    assert(!xm_bset_has(&bs, 64));
    assert(!xm_bset_has(&bs, 255));

    xm_bset_set(&bs, 0);
    xm_bset_set(&bs, 63);
    xm_bset_set(&bs, 64);
    xm_bset_set(&bs, 255);

    assert(xm_bset_has(&bs, 0));
    assert(xm_bset_has(&bs, 63));
    assert(xm_bset_has(&bs, 64));
    assert(xm_bset_has(&bs, 255));
    assert(!xm_bset_has(&bs, 1));
    assert(!xm_bset_has(&bs, 128));

    assert(xm_bset_count(&bs) == 4);

    xm_bset_clr(&bs, 63);
    assert(!xm_bset_has(&bs, 63));
    assert(xm_bset_count(&bs) == 3);

    xm_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_union(void) {
    fprintf(stderr, "  test_union...");
    XmBSet a, b;
    xm_bset_init(&a, 128);
    xm_bset_init(&b, 128);

    xm_bset_set(&a, 0);
    xm_bset_set(&a, 10);
    xm_bset_set(&b, 10);
    xm_bset_set(&b, 100);

    xm_bset_union(&a, &b);
    assert(xm_bset_has(&a, 0));
    assert(xm_bset_has(&a, 10));
    assert(xm_bset_has(&a, 100));
    assert(xm_bset_count(&a) == 3);

    xm_bset_free(&a);
    xm_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_inter(void) {
    fprintf(stderr, "  test_inter...");
    XmBSet a, b;
    xm_bset_init(&a, 128);
    xm_bset_init(&b, 128);

    xm_bset_set(&a, 0);
    xm_bset_set(&a, 10);
    xm_bset_set(&a, 50);
    xm_bset_set(&b, 10);
    xm_bset_set(&b, 50);
    xm_bset_set(&b, 100);

    xm_bset_inter(&a, &b);
    assert(!xm_bset_has(&a, 0));
    assert(xm_bset_has(&a, 10));
    assert(xm_bset_has(&a, 50));
    assert(!xm_bset_has(&a, 100));
    assert(xm_bset_count(&a) == 2);

    xm_bset_free(&a);
    xm_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_diff(void) {
    fprintf(stderr, "  test_diff...");
    XmBSet a, b;
    xm_bset_init(&a, 128);
    xm_bset_init(&b, 128);

    xm_bset_set(&a, 0);
    xm_bset_set(&a, 10);
    xm_bset_set(&a, 50);
    xm_bset_set(&b, 10);

    xm_bset_diff(&a, &b);
    assert(xm_bset_has(&a, 0));
    assert(!xm_bset_has(&a, 10));
    assert(xm_bset_has(&a, 50));
    assert(xm_bset_count(&a) == 2);

    xm_bset_free(&a);
    xm_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_copy(void) {
    fprintf(stderr, "  test_copy...");
    XmBSet a, b;
    xm_bset_init(&a, 128);
    xm_bset_init(&b, 128);

    xm_bset_set(&a, 5);
    xm_bset_set(&a, 77);
    xm_bset_copy(&b, &a);
    assert(xm_bset_has(&b, 5));
    assert(xm_bset_has(&b, 77));
    assert(xm_bset_equal(&a, &b));

    xm_bset_free(&a);
    xm_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_equal(void) {
    fprintf(stderr, "  test_equal...");
    XmBSet a, b;
    xm_bset_init(&a, 128);
    xm_bset_init(&b, 128);

    assert(xm_bset_equal(&a, &b));

    xm_bset_set(&a, 42);
    assert(!xm_bset_equal(&a, &b));

    xm_bset_set(&b, 42);
    assert(xm_bset_equal(&a, &b));

    xm_bset_free(&a);
    xm_bset_free(&b);
    fprintf(stderr, " PASS\n");
}

static void test_iter(void) {
    fprintf(stderr, "  test_iter...");
    XmBSet bs;
    xm_bset_init(&bs, 256);

    xm_bset_set(&bs, 3);
    xm_bset_set(&bs, 64);
    xm_bset_set(&bs, 130);
    xm_bset_set(&bs, 200);

    int iter = 0;
    int bit;
    int collected[4];
    int n = 0;
    while ((bit = xm_bset_iter(&bs, &iter)) >= 0) {
        assert(n < 4);
        collected[n++] = bit;
    }
    assert(n == 4);
    assert(collected[0] == 3);
    assert(collected[1] == 64);
    assert(collected[2] == 130);
    assert(collected[3] == 200);

    xm_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_iter_empty(void) {
    fprintf(stderr, "  test_iter_empty...");
    XmBSet bs;
    xm_bset_init(&bs, 128);

    int iter = 0;
    assert(xm_bset_iter(&bs, &iter) == -1);

    xm_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_fill(void) {
    fprintf(stderr, "  test_fill...");
    XmBSet bs;
    xm_bset_init(&bs, 128);

    xm_bset_fill(&bs);
    assert(xm_bset_has(&bs, 0));
    assert(xm_bset_has(&bs, 63));
    assert(xm_bset_has(&bs, 64));
    assert(xm_bset_has(&bs, 127));
    assert(xm_bset_count(&bs) == 128);

    xm_bset_zero(&bs);
    assert(xm_bset_empty(&bs));

    xm_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

static void test_large(void) {
    fprintf(stderr, "  test_large...");
    XmBSet bs;
    xm_bset_init(&bs, 4096);

    xm_bset_set(&bs, 0);
    xm_bset_set(&bs, 4095);
    xm_bset_set(&bs, 2048);
    assert(xm_bset_count(&bs) == 3);
    assert(xm_bset_has(&bs, 4095));

    xm_bset_free(&bs);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xm_bset ===\n");

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
