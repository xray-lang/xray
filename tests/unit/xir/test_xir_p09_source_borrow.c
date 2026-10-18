/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_source_borrow.c - Exclusive call roots and captured mutable bindings
 *
 * KEY CONCEPT:
 *   Known conflicts fail before publication while independent value owners run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
#include "xir/xxir_program.h"

static uint32_t borrow_entry(const XrXirModule *module) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"entry",5)) return f;
    CHECK(false); return UINT32_MAX;
}
#include "p09_source_borrow_cases.h"

int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirSourceResult result={0};
    CHECK(hash_source_check(&owner.context,"export fn entry()->i64{return 41}",&result)==XR_XIR_OK);
    borrow_source_run(hash_source_replay(&owner.context,&result),41);
    source_fixture_owner_free(&owner);
    source_borrow_cases();
    return 0;
}
