/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_combined_identity.c - Complete parent packets stay outside the new reader
 *
 * KEY CONCEPT:
 *   Independently framed whole packets retain their hashes and fail before
 *   allocation; the combined Core model is independently read and written.
 */
#include "xir/xxir_checked.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir_assert_panics_current_golden.h"
#include "xir_assert_panics_ctfe75_golden.h"
#include "xir_assert_panics_channel76_golden.h"
#include "xir_channel_types76_golden.h"
#include "xir_channel_blocking76_golden.h"
#include "xir_channel_timed76_golden.h"
#include "compile_owner/checked_sizing75_golden.h"
#include "comptime_binding75_golden.h"
#include "materialization75_golden.h"
#include "root_construction75_goldens.h"
#include "xir_checked_range75_golden.h"
#include "xir_checked_scalar75_golden.h"
#include "xir_defaults75_golden_bytes.h"
#include "xir_error_marker75_golden.h"
#include "xir_generic_method75_golden.h"
#include "xir_library_string75_goldens.h"
#include "xir_rune75_golden.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "xir_source_compile_owner.h"
typedef struct ParentPacket { const uint8_t *bytes; size_t length; } ParentPacket;
#define PARENT(name) {name,sizeof(name)}

static void reject_parent(const XrXirCompileContext *context, ParentPacket parent) {
    CHECK(parent.length>64 && !memcmp(parent.bytes,"XRCHK",5));
    XrSHA256Context hash;uint8_t digest[32];xr_sha256_init(&hash);
    xr_sha256_update(&hash,parent.bytes,32);xr_sha256_update(&hash,parent.bytes+64,parent.length-64);
    xr_sha256_final(&hash,digest);CHECK(!memcmp(digest,parent.bytes+32,32));
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *sentinel=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*output=sentinel;
        CHECK(xr_xir_compile_checked_read(context,parent.bytes,parent.length,&output,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==sentinel);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        /* The empty-output read charges its exact32 header bytes; an occupied
         * output rejects before reading. Neither path allocates or refunds. */
        CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes && after.work==before.work+32);
    }
}

int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==30 && XR_XIR_CHECKED_CONTRACT==77,"combined format identity");
    const ParentPacket parents[]={
        PARENT(assert_panics_ctfe75_golden),
        PARENT(assert_panics_channel76_golden),
        PARENT(channel_types76_golden),
        PARENT(channel_unit76_golden),
        PARENT(blocking_send_i6476_golden),
        PARENT(blocking_send_unit76_golden),
        PARENT(blocking_recv_or_i6476_golden),
        PARENT(blocking_recv_or_unit76_golden),
        PARENT(retained_send_i6476_golden),
        PARENT(retained_send_unit76_golden),
        PARENT(retained_recv_or_i6476_golden),
        PARENT(retained_recv_or_unit76_golden),
        PARENT(timed_send_i6476_golden),
        PARENT(timed_send_unit76_golden),
        PARENT(timed_recv_i6476_golden),
        PARENT(timed_recv_unit76_golden),
        PARENT(sizing75_golden),
        PARENT(sizing75_embedded_golden),
        PARENT(sizing75_empty_golden),
        PARENT(comptime_binding75_golden),
        PARENT(materialization75_golden),
        PARENT(root_construction75_checked),
        PARENT(root_construction75_instance),
        PARENT(root_construction71_previous),
        PARENT(checked_range75_golden),
        PARENT(checked_scalar75_golden),
        PARENT(defaults75_golden_7),
        PARENT(defaults75_golden_8),
        PARENT(defaults75_golden_9),
        PARENT(defaults75_golden_10),
        PARENT(defaults75_invoke_golden),
        PARENT(error_marker75_golden),
        PARENT(error_marker73_previous),
        PARENT(generic_method75_golden),
        PARENT(generic_method75_invalid_flags0),
        PARENT(generic_method75_invalid_flags1),
        PARENT(library_string75_alpha),
        PARENT(library_string75_beta),
        PARENT(library_string75_nul),
        PARENT(library_string75_empty),
        PARENT(library_string75_long),
        PARENT(library_string75_equal),
        PARENT(library_string75_unused),
        PARENT(rune75_golden),
    };
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(parents)/sizeof(parents[0]);++i) reject_parent(&context,parents[i]);
    XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_read(&context,assert_panics_current_golden,sizeof(assert_panics_current_golden),&artifact,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(assert_panics_current_golden) && !memcmp(packet.bytes,assert_panics_current_golden,packet.length));
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);
    XrCompileResourceStats final={0};CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Combined30/77 independent Core roundtrip and44 complete parent packets: unchanged outputs and exact32-work no-allocation early rejection PASS");
    return 0;
}
