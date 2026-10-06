/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_nominal_constructors.c - Original deep class source producer
 *
 * KEY CONCEPT:
 *   The exact original C loop independently produces all eighty class declarations.
 */
#include "base/xmalloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ASSERT_TRUE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define ASSERT_NOT_NULL(c) ASSERT_TRUE((c) != NULL)
static void emit_deep_class_graph(FILE *output) {

    enum {
        CLASS_COUNT = 80,
        SOURCE_CAPACITY = 32768
    };
    char *source = xr_calloc(SOURCE_CAPACITY, 1u);
    ASSERT_NOT_NULL(source);
    size_t used = 0u;
    for (int32_t index = CLASS_COUNT - 1; index >= 0; --index) {
        int written = index == CLASS_COUNT - 1
                          ? snprintf(source + used, SOURCE_CAPACITY - used,
                                     "class C%d {\n  value: i64\n  constructor(value: i64) { "
                                     "this.value = value }\n}\n",
                                     index)
                          : snprintf(source + used, SOURCE_CAPACITY - used,
                                     "class C%d {\n  next: C%d?\n  constructor(next: C%d?) { "
                                     "this.next = next }\n}\n",
                                     index, index + 1, index + 1);
        ASSERT_TRUE(written > 0 && (size_t) written < SOURCE_CAPACITY - used);
        used += (size_t) written;
    }
    int written = snprintf(source + used, SOURCE_CAPACITY - used,
                           "fn answer(root: C0) -> i64 {\n  return 42\n}\n");
    ASSERT_TRUE(written > 0 && (size_t) written < SOURCE_CAPACITY - used);

    ASSERT_TRUE(fwrite(source, 1, strlen(source), output) == strlen(source));
    xr_free(source);
}
int main(int argc, char **argv) {
    ASSERT_TRUE(argc == 3);
    ASSERT_TRUE(!strcmp(argv[1], "deep_class_graph"));
    FILE *output = fopen(argv[2], "wb");
    ASSERT_NOT_NULL(output);
    emit_deep_class_graph(output);
    ASSERT_TRUE(!fclose(output));
    return 0;
}
