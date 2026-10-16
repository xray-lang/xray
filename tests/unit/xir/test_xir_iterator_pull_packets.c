/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_iterator_pull_packets.c - Independent typed pull CFG receiving
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_value.h"
#include "xir/xxir_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_iterator_pull86_golden.h"
_Static_assert(XR_XIR_INVOKE_RESULT==101 && XR_XIR_INVOKE_ERROR==102 && XR_XIR_ERROR==14,
    "independent control and Error ordinals");
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    const struct {const uint8_t *bytes;size_t length;XrXirStatus status;} cases[]={
#define ROW(n,s) {pull86_##n,sizeof(pull86_##n),s}
        ROW(has_next_i64,XR_XIR_OK),ROW(next_i64,XR_XIR_OK),ROW(has_next_unit,XR_XIR_OK),ROW(next_unit,XR_XIR_OK),
        ROW(wrong_receiver,XR_XIR_BAD_TYPE),ROW(wrong_result,XR_XIR_BAD_STRUCTURE),
        ROW(same_successor,XR_XIR_BAD_STRUCTURE),ROW(wrong_origin,XR_XIR_BAD_STRUCTURE),ROW(unused_immediate,XR_XIR_BAD_STRUCTURE)
#undef ROW
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrXirArtifact *artifact=NULL;XrXirStatus status=xr_xir_compile_checked_read(&owner.context,
            cases[i].bytes,cases[i].length,&artifact,NULL);
        if(status!=cases[i].status) fprintf(stderr,"Pull packet%u status%u expected%u\n",i,status,cases[i].status);
        CHECK(status==cases[i].status);
        if(status==XR_XIR_OK) {
            XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
            XrXirEffectFacts facts=xr_xir_effects_read(effects,0);
            CHECK(facts.status==XR_XIR_OK && facts.control.suspend==XR_XIR_EFFECT_MAY &&
                facts.suspend_witness.cause==XR_XIR_EFFECT_CAUSE_SUSPEND && !facts.suspend_witness.instruction);
            xr_xir_compile_effects_free(effects);
            XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
            CHECK(packet.length==cases[i].length && !memcmp(packet.bytes,cases[i].bytes,packet.length));
            xr_xir_compile_checked_packet_free(&packet);XrXirArtifact *lowered=NULL;
            CHECK(xr_xir_compile_lower(artifact,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);
        } else CHECK(!artifact);
        xr_xir_compile_artifact_free(artifact);
    }
    source_fixture_owner_free(&owner);puts("Independent pull packets: four exact roundtrips and five exact refusals PASS");return 0;
}
