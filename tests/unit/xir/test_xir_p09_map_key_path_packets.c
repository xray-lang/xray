/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_packets.c - Independent typed path and worker admission
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir/xxir_checked.h"
#include "hash_protocol28_inputs/xir_map_key_path82_golden.h"
#include "composed_wire/packet_076.inc.c"
#include "composed_wire/packet_077.inc.c"
#include "composed_wire/packet_078.inc.c"
#include "composed_wire/packet_079.inc.c"
#include "composed_wire/packet_080.inc.c"
#include "composed_wire/packet_081.inc.c"
#include "composed_wire/packet_082.inc.c"
#include "composed_wire/packet_083.inc.c"
#include "composed_wire/packet_084.inc.c"
#include "composed_wire/packet_085.inc.c"
#include "hash_protocol28_inputs/p09_hash_retired82_packet_table.h"
#include "p09_hash_current84_packet_table.h"

typedef struct KeyPacketCase {
    const char *name;
    const uint8_t *bytes;
    size_t length;
    XrXirStatus expected;
} KeyPacketCase;
#define PACKET_CASE(name,packet,status) {#name,packet,sizeof(packet),status}
int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==34 && XR_XIR_CHECKED_CONTRACT==90 && XR_XIR_OP_COUNT==193,"typed key path identity");
    SourceFixtureOwner owner={0}; source_fixture_owner_new(&owner);
    const KeyPacketCase cases[]={
        PACKET_CASE(path_i64,packet_076_current,XR_XIR_OK),PACKET_CASE(path_string,packet_077_current,XR_XIR_OK),
        PACKET_CASE(map_go,packet_083_current,XR_XIR_OK),PACKET_CASE(set_go,packet_084_current,XR_XIR_OK),
        PACKET_CASE(bad_map_key,packet_078_current,XR_XIR_BAD_TYPE),PACKET_CASE(bad_map_result,packet_079_current,XR_XIR_BAD_TYPE),
        PACKET_CASE(bad_array_key,packet_080_current,XR_XIR_BAD_TYPE),PACKET_CASE(bad_set_parent,packet_081_current,XR_XIR_BAD_TYPE),
        PACKET_CASE(bad_value_parent,packet_082_current,XR_XIR_BAD_VALUE),PACKET_CASE(bad_map_go_callable_value,packet_085_current,XR_XIR_BAD_TYPE)};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrXirArtifact *artifact=NULL; XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_checked_read(&owner.context,cases[i].bytes,cases[i].length,&artifact,&diagnostic);
        printf("independent key packet %s status=%u expected=%u function=%u instruction=%u\n",
            cases[i].name,status,cases[i].expected,diagnostic.function,diagnostic.instruction);
        CHECK(status==cases[i].expected && (artifact!=NULL)==(status==XR_XIR_OK));
        if(artifact) {
            XrXirCheckedPacket packet={0};
            CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
            CHECK(packet.length==cases[i].length && !memcmp(packet.bytes,cases[i].bytes,packet.length));
            xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(artifact);
        }
    }
    _Static_assert(sizeof(current_hash84_packets)/sizeof(current_hash84_packets[0])==60,"complete field sample count");
    for(unsigned i=0;i<60;++i) {
        const CurrentHashPacket *row=&current_hash84_packets[i];
        XrCompileResourceStats baseline={0},after={0};
        CHECK(xr_compile_resources_stats(owner.context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *artifact=NULL;
        XrXirStatus status=xr_xir_compile_checked_read(&owner.context,row->bytes,row->length,&artifact,NULL);
        printf("complete field %s status=%u expected=%u\n",row->name,status,row->expected);
        CHECK(status==row->expected && (artifact!=NULL)==(status==XR_XIR_OK));
        if(artifact) {
            XrXirCheckedPacket packet={0};
            CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
            CHECK(packet.length==row->length && !memcmp(packet.bytes,row->bytes,row->length));
            xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);
        }
        CHECK(xr_compile_resources_stats(owner.context.resources,&after)==XR_COMPILE_RESOURCE_OK &&
            after.live_bytes==baseline.live_bytes);
        XrXirArtifact *sentinel=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(&owner.context,row->bytes,row->length,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE &&
            sentinel==(XrXirArtifact *)(uintptr_t)1);
        XrCompileResourceStats occupied={0};
        CHECK(xr_compile_resources_stats(owner.context.resources,&occupied)==XR_COMPILE_RESOURCE_OK &&
            occupied.allocation_count==after.allocation_count && occupied.allocated_bytes==after.allocated_bytes &&
            occupied.live_bytes==after.live_bytes && occupied.work==after.work);
    }
    const uint8_t *old[]={map_key_path82_map_go,map_key_path82_set_go,map_key_path82_bad_map_go_callable_value};
    const size_t sizes[]={sizeof(map_key_path82_map_go),sizeof(map_key_path82_set_go),sizeof(map_key_path82_bad_map_go_callable_value)};
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(owner.context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<3;++i)for(unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *artifact=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*saved=artifact;
        CHECK(xr_xir_compile_checked_read(&owner.context,old[i],sizes[i],&artifact,NULL)==XR_XIR_BAD_STRUCTURE && artifact==saved);
    }
    _Static_assert(sizeof(retired_hash82_packets)/sizeof(retired_hash82_packets[0])==60,"complete prior field sample count");
    for(unsigned i=0;i<60;++i)for(unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *artifact=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*saved=artifact;
        CHECK(xr_xir_compile_checked_read(&owner.context,retired_hash82_packets[i].bytes,
            retired_hash82_packets[i].length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE && artifact==saved);
    }
    CHECK(xr_compile_resources_stats(owner.context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes);
    source_fixture_owner_free(&owner);
    puts("Typed Map path and worker packet fields; complete retired worker frames rejected before allocation PASS");return 0;
}
