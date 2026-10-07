/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_execution_native_producer.c - Real generated root programs
 *
 * KEY CONCEPT:
 *   All generated entries retain their original checked identity and body.
 */
#include "xir/xxir_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_source_fixture_owner.h"
#include "xir_root_execution_native_pipeline.h"

int main(int argc, char **argv) {
    CHECK(argc == 6);
    instance_compile_zero();
    SourceFixtureOwner owner = {0};
    source_fixture_owner_new(&owner);
    FILE *header = fopen(argv[5], "wb");
    CHECK(header && fputs("/* Real generated Program identities; independent outcomes live in the consumer. */\n", header) >= 0);
    for (unsigned fixture = 0; fixture < 4; ++fixture) rn_emit_fixture(&owner.context, fixture, argv[fixture + 1], header);
    CHECK(!fclose(header));
    source_fixture_owner_free(&owner);
    instance_compile_report();
    return 0;
}
