/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_array_hold_source.c - Lowered local Arrays become held chains
 */
#include "lir_source_fixture.h"
#include "lir/xlir_array_hold.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_print.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Text { char *bytes; size_t length, capacity; } Text;
static bool text_write(void *context, const char *bytes, size_t length) {
    Text *t = context;
    if (t->length + length + 1 > t->capacity) {
        size_t capacity = (t->length + length + 1) * 2;
        char *grown = realloc(t->bytes, capacity);
        if (!grown) return false;
        t->bytes = grown; t->capacity = capacity;
    }
    memcpy(t->bytes + t->length, bytes, length); t->length += length; t->bytes[t->length] = 0;
    return true;
}

/* The held LIR of a whole program; the transformation verifies its output. */
static Text held_program(const char *text) {
    SourceFixture source = source_fixture(text);
    XrLirLowerRequest request = {source.closed, source.source.snapshot, false};
    XrLirLowered lowered = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_program_checked(&request, &lowered, &failure);
    if (status != XR_LIR_OK) fprintf(stderr, "lower: %s\n", failure.diagnostic.message);
    CHECK(status == XR_LIR_OK);
    XrLirModule *held = NULL;
    CHECK(xr_lir_array_hold(lowered.module, &held) == XR_LIR_OK && held);
    Text printed = {0};
    CHECK(xr_lir_print(held, text_write, &printed) == XR_LIR_OK);
    xr_lir_module_destroy(held);
    xr_lir_lowered_free(&lowered);
    xr_lir_lower_failure_free(&failure);
    source_fixture_free(&source);
    return printed;
}
/* The body of one printed function. */
static char *function_text(const Text *printed, const char *name) {
    const char *start = strstr(printed->bytes, name);
    CHECK(start);
    const char *end = strstr(start, "\n  }");
    CHECK(end);
    size_t length = (size_t)(end - start);
    char *copy = malloc(length + 1);
    CHECK(copy);
    memcpy(copy, start, length); copy[length] = 0;
    return copy;
}
static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) ++count;
    return count;
}

/* Every element read, write, push and length of one local Array is held; the
 * Cell round trips of its mutations disappear; one hold and one unhold remain. */
static void whole_function_chain(void) {
    Text printed = held_program(
        "fn work(n: i64) -> i64 {\n"
        "    var a: Array<i64> = []\n"
        "    var i = 0\n"
        "    while (i < n) { a.push(i); i = i + 1 }\n"
        "    i = 0\n"
        "    while (i < len(a)) { a[i] = a[i] * 3 + 1; i = i + 1 }\n"
        "    var total = 0\n"
        "    i = 0\n"
        "    while (i < len(a)) { total = total + a[i]; i = i + 1 }\n"
        "    return total\n"
        "}\nprint(work(10))\n");
    char *work = function_text(&printed, "func @f1_work");
    CHECK(occurrences(work, "array_hold ") == 1 && occurrences(work, "array_unhold") == 1);
    CHECK(occurrences(work, "array_held_push") == 1 && occurrences(work, "array_held_set") == 1 &&
        occurrences(work, "array_held_get") == 2 && occurrences(work, "array_held_len") == 2);
    CHECK(!strstr(work, "cell_new") && !strstr(work, "cell_read") && !strstr(work, "call_intrinsic #2(") &&
        strstr(work, "array held"));
    free(work); free(printed.bytes);
}

/* Passing the Array to a call is an alias: it sees the unheld owner and the
 * chain is held again after the call. The callee's borrowed parameter is not
 * an owner and stays unheld. */
static void call_alias_in_loop(void) {
    Text printed = held_program(
        "fn total(a: Array<i64>) -> i64 {\n"
        "    var s = 0\n"
        "    var i = 0\n"
        "    while (i < len(a)) { s = s + a[i]; i = i + 1 }\n"
        "    return s\n"
        "}\n"
        "fn work(n: i64) -> i64 {\n"
        "    var a: Array<i64> = []\n"
        "    var s = 0\n"
        "    var i = 0\n"
        "    while (i < n) { a.push(i); s = s + total(a); i = i + 1 }\n"
        "    return s\n"
        "}\nprint(work(10))\n");
    char *total = function_text(&printed, "func @f1_total");
    char *work = function_text(&printed, "func @f2_work");
    CHECK(!strstr(total, "array_hold") && !strstr(total, "array_held"));
    CHECK(occurrences(work, "array_held_push") == 1 && occurrences(work, "array_hold ") == 2 &&
        occurrences(work, "array_unhold") == 2);
    const char *call = strstr(work, "call @f1_total");
    CHECK(call);
    const char *unhold = strstr(work, "array_unhold");
    const char *again = strstr(call, "array_hold ");
    CHECK(unhold && unhold < call && again);
    free(total); free(work); free(printed.bytes);
}

/* String elements are released one by one on cleanup and are not held. */
static void string_elements_unheld(void) {
    Text printed = held_program(
        "fn names(n: i64) -> i64 {\n"
        "    var a: Array<string> = []\n"
        "    var i = 0\n"
        "    while (i < n) { a.push(\"x\"); i = i + 1 }\n"
        "    return len(a)\n"
        "}\nprint(names(3))\n");
    CHECK(!strstr(printed.bytes, "array_hold") && !strstr(printed.bytes, "array_held"));
    free(printed.bytes);
}

int main(void) {
    whole_function_chain();
    call_alias_in_loop();
    string_elements_unheld();
    puts("lowered local Arrays: whole-function chains held, call aliases unheld and held again, String elements unheld");
    return 0;
}
