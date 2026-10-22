/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_locations.c - Production location owner lifetime and invalid inputs
 */
#include "runtime/native/xnative_locations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void owned_bytes(void) {
    char path[] = {'x', '\0', '\n', (char)0xc3, (char)0xa9, '!'};
    const char expected[] = {'x', '\0', '\n', (char)0xc3, (char)0xa9};
    XrtSourceLocation input[] = {{path, sizeof(path) - 1, 3, 5, 0, 0},
        {path, sizeof(path) - 1, 7, 9, 8, 2}};
    {
        memcpy(path, expected, sizeof(expected));
        XrtLocationOwner *owner = NULL;
        CHECK(xrt_locations_create(input, 2, &owner) == XRT_LOCATION_OK);
        CHECK(xrt_locations_count(owner) == 2 && xrt_locations_retain(owner));
        memset(path, '?', sizeof(path));
        xrt_locations_release(owner);
        const XrtSourceLocation *first = xrt_locations_get(owner, 1);
        const XrtSourceLocation *last = xrt_locations_get(owner, 2);
        CHECK(first && first->path != path && first->path_length == sizeof(expected));
        CHECK(!memcmp(first->path, expected, sizeof(expected)) && first->line == 3 &&
            first->column == 5 && !first->end_line && !first->end_column);
        CHECK(last && last->line == 7 && last->column == 9 && last->end_line == 8 &&
            last->end_column == 2 && !memcmp(last->path, expected, sizeof(expected)));
        CHECK(!xrt_locations_get(owner, 0) && !xrt_locations_get(owner, 3));
        xrt_locations_release(owner);
    }
}

static void invalid_and_empty(void) {
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK && owner);
    CHECK(!xrt_locations_count(owner) && !xrt_locations_get(owner, 1));
    XrtLocationOwner *same = owner;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_INVALID && owner == same);
    xrt_locations_release(owner);
    owner = NULL;
    XrtSourceLocation value = {"point", 5, 1, 1, 0, 9};
    CHECK(xrt_locations_create(&value, 1, &owner) == XRT_LOCATION_INVALID && !owner);
    value.end_line = 1;
    value.end_column = 0;
    CHECK(xrt_locations_create(&value, 1, &owner) == XRT_LOCATION_INVALID && !owner);
    value.end_line = 0;
    CHECK(xrt_locations_create(&value, 0, &owner) == XRT_LOCATION_INVALID && !owner);
    CHECK(xrt_locations_create(NULL, 1, &owner) == XRT_LOCATION_INVALID && !owner);
    CHECK(xrt_locations_create(NULL, 0, NULL) == XRT_LOCATION_INVALID);
    CHECK(!xrt_locations_retain(NULL) && !xrt_locations_count(NULL));
    CHECK(!xrt_locations_get(NULL, 0));
    xrt_locations_release(NULL);
}

int main(void) {
    owned_bytes();
    invalid_and_empty();
    puts("Native location ownership passed");
    return 0;
}
