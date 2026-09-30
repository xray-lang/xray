/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtoml.c - TOML duplicate assignment and insertion ownership checks
 */
#include "base/xtoml.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char *const rejected[] = {
        "key=1\nkey=2\n",
        "key=1\n\"key\"=2\n",
        "a.b=1\na.b=2\n",
        "a=1\na.b=2\n",
        "a.b=1\na=2\n",
        "a={b=1,b=2}\n",
        "a={b=1,b.c=2}\n",
        "[native.symbol.contract]\nsuspend='never'\nsuspend='may'\n"
    };
    static const char *const accepted[] = {
        "a.b=1\na.c=2\n",
        "[[items]]\nx=1\n[[items]]\nx=2\n",
        "a={b=1,c=2}\n",
        "[a.b]\nx=1\n[a]\ny=2\n"
    };
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(rejected)/sizeof(rejected[0]); ++i) {
        XrTomlValue *value = xtoml_parse(rejected[i], strlen(rejected[i]));
        if (value) {
            fprintf(stderr, "accepted conflicting assignment %zu\n", i);
            ++failures;
        }
        xtoml_free(value);
    }
    for (size_t i = 0; i < sizeof(accepted)/sizeof(accepted[0]); ++i) {
        XrTomlValue *value = xtoml_parse(accepted[i], strlen(accepted[i]));
        if (!value) {
            fprintf(stderr, "rejected valid document %zu\n", i);
            ++failures;
        }
        xtoml_free(value);
    }
    printf("TOML assignment checks: %u failures\n", failures);
    return failures ? 1 : 0;
}
