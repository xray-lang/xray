/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_defaults_wire_cases.h - Owned parameter defaults and canonical packet boundaries
 *
 * KEY CONCEPT:
 *   Independent bytes survive producer destruction and every partial allocation failure.
 */
#ifndef XIR_DEFAULTS_WIRE_CASES_H
#define XIR_DEFAULTS_WIRE_CASES_H
#include "xir_defaults_golden_bytes.h"
#include "xir_defaults_invoke_golden.h"
static void defaults_wire_goldens(void) {
    struct { const uint8_t *bytes; size_t length; bool valid; } cases[] = {
        {defaults_golden_0,sizeof(defaults_golden_0),false},
        {defaults_golden_1,sizeof(defaults_golden_1),false},
        {defaults_golden_2,sizeof(defaults_golden_2),false},
        {defaults_golden_3,sizeof(defaults_golden_3),false},
        {defaults_golden_4,sizeof(defaults_golden_4),false},
        {defaults_golden_5,sizeof(defaults_golden_5),false},
        {defaults_golden_6,sizeof(defaults_golden_6),false},
        {defaults_golden_7,sizeof(defaults_golden_7),true},
        {defaults_golden_8,sizeof(defaults_golden_8),true},
        {defaults_golden_9,sizeof(defaults_golden_9),true},
        {defaults_golden_10,sizeof(defaults_golden_10),true}
    };
    size_t live = runtime_live, physical = runtime_bytes;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        XrXirArtifact *artifact = NULL;
        XrXirStatus status = xr_xir_compile_checked_read(library_context,cases[i].bytes,cases[i].length,&artifact,NULL);
        CHECK(status == (cases[i].valid ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE));
        if (cases[i].valid) {
            XrXirCheckedPacket packet = {0};
            CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL) == XR_XIR_OK);
            CHECK(packet.length == cases[i].length && !memcmp(packet.bytes,cases[i].bytes,packet.length));
            xr_xir_compile_checked_packet_free(&packet);
        } else CHECK(!artifact);
        xr_xir_compile_artifact_free(artifact);
        CHECK(runtime_live == live && runtime_bytes == physical);
    }
}
static void defaults_wire_ownership(void) {
    size_t live = runtime_live, physical = runtime_bytes;
    XrXirArtifact *producer = NULL, *copy = NULL;
    CHECK(xr_xir_compile_checked_read(library_context,defaults_golden_7,sizeof(defaults_golden_7),&producer,NULL) == XR_XIR_OK);
    const XrXirModule *original = xr_xir_compile_artifact_module(producer);
    CHECK(original->defaults && original->defaults->count == 1);
    CHECK(original->defaults->records[0].function == 0 && original->defaults->records[0].owner == 2);
    CHECK(xr_xir_compile_recheck(library_context,original,&copy,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(copy)->defaults != original->defaults);
    CHECK(xr_xir_compile_artifact_module(copy)->defaults->records != original->defaults->records);
    xr_xir_compile_artifact_free(producer);
    CHECK(xr_xir_compile_artifact_verify(copy,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(copy,&packet,NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(defaults_golden_7) && !memcmp(packet.bytes,defaults_golden_7,packet.length));
    xr_xir_compile_artifact_free(copy); xr_xir_compile_checked_packet_free(&packet);
    CHECK(runtime_live == live && runtime_bytes == physical);
}
static void defaults_clone_failures(void) {
    size_t live = runtime_live, physical = runtime_bytes;
    XrXirArtifact *producer = NULL, *copy = NULL;
    CHECK(xr_xir_compile_checked_read(library_context,defaults_golden_7,sizeof(defaults_golden_7),&producer,NULL) == XR_XIR_OK);
    const XrXirModule *original = xr_xir_compile_artifact_module(producer);
    LibraryPacketFixture fixture={NULL,0,original};library_compile_operation_cases("defaults owned clone",library_packet_operation,&fixture);
    XrXirModule bad = *original;
    XrXirDefaultTable empty = {0}; bad.defaults = &empty;
    CHECK(xr_xir_compile_recheck(library_context,&bad,&copy,NULL) == XR_XIR_BAD_STRUCTURE && !copy);
    xr_xir_compile_artifact_free(producer);
    CHECK(runtime_live == live && runtime_bytes == physical);
    puts("defaults clone all compiler OOM/three axes physical baseline");
}
static void defaults_wire_failure_prefix(void) {
    size_t live = runtime_live, physical = runtime_bytes;
    XrXirArtifact *artifact = NULL;
    LibraryPacketFixture fixture={defaults_golden_7,sizeof(defaults_golden_7),NULL};
    library_compile_operation_cases("defaults packet reader/writer",library_packet_operation,&fixture);
    uint8_t bad[sizeof(defaults_golden_7)];
    memcpy(bad,defaults_golden_7,sizeof(bad));
    /* The independent fixture's count is at 542, before its 20-byte record. */
    memset(bad+542,255,4);
    XrSHA256Context digest; xr_sha256_init(&digest); xr_sha256_update(&digest,bad,32);
    xr_sha256_update(&digest,bad+64,sizeof(bad)-64); xr_sha256_final(&digest,bad+32);
    CHECK(xr_xir_compile_checked_read(library_context,bad,sizeof(bad),&artifact,NULL) == XR_XIR_BAD_STRUCTURE && !artifact);
    CHECK(runtime_live == live && runtime_bytes == physical);
    for (size_t length = 518; length < sizeof(bad); ++length) {
        memcpy(bad,defaults_golden_7,sizeof(bad));
        uint64_t payload = length - 64;
        for (unsigned j = 0; j < 8; ++j) bad[24+j] = (uint8_t)(payload >> (j*8));
        xr_sha256_init(&digest); xr_sha256_update(&digest,bad,32);
        xr_sha256_update(&digest,bad+64,length-64); xr_sha256_final(&digest,bad+32);
        CHECK(xr_xir_compile_checked_read(library_context,bad,length,&artifact,NULL) == XR_XIR_BAD_STRUCTURE && !artifact);
        CHECK(runtime_live == live && runtime_bytes == physical);
    }
    for (unsigned field = 0; field < 4; ++field) {
        memcpy(bad,defaults_golden_7,sizeof(bad));
        memset(bad+546+field*4,255,4);
        xr_sha256_init(&digest); xr_sha256_update(&digest,bad,32);
        xr_sha256_update(&digest,bad+64,sizeof(bad)-64); xr_sha256_final(&digest,bad+32);
        XrXirStatus status=xr_xir_compile_checked_read(library_context,bad,sizeof(bad),&artifact,NULL);
        if(status!=XR_XIR_BAD_STRUCTURE||artifact)fprintf(stderr,"defaults corrupt field%u status%u out%p\n",field,status,(void *)artifact);
        CHECK(status==XR_XIR_BAD_STRUCTURE&&!artifact);
        CHECK(runtime_live == live && runtime_bytes == physical);
    }
    puts("defaults wire full compiler OOM/three axes; owned table and truncation physicalzero");
}
static void defaults_wire_cases(void) {
    defaults_invoke_golden_cases(); defaults_wire_goldens(); defaults_wire_ownership(); defaults_clone_failures(); defaults_wire_failure_prefix();
}
#endif // XIR_DEFAULTS_WIRE_CASES_H
