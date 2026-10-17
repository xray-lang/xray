/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_source_compiler.c - Single-owner Atomic Source and code preparation
 */
#include "xir_mixed_atomic_fixture.h"
int main(int argc,char **argv) {
    CHECK(argc==2);const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    AtomicRuntimeFixture fixture={0};XrXirProgram *program=NULL;
    bool complete=atomic_runtime_prepare(&fixture,backend,true,&program);if(program)xr_xir_compile_program_drop(program);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(fixture.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    size_t sites=effects_compile_attempts;effects_source_owners_free();CHECK(!runtime_live && !runtime_bytes);
    printf("Atomic full Source backend=%u complete=%u stage=%u status=%u sites=%zu allocated=%llu peak=%llu work=%llu codeLeases=%u cbytes=%zu cBodyMatched=%u vmPrepared=%u sealed=%u sameSourceContext physical0/0\n",
        backend,(unsigned)complete,fixture.stage,(unsigned)fixture.status,sites,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,fixture.releases,p20_atomic_runtime_c_bytes,(unsigned)fixture.c_body_matched,(unsigned)fixture.vm_prepared,(unsigned)complete);
    CHECK(complete && fixture.stage==11 && fixture.status==XR_XIR_OK && fixture.releases==1 && sites<20000);return 0;
}
