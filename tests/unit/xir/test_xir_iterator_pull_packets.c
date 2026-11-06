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
#include "xir_construction_fixture.h"
/* These are current typed CFG inputs, not external packet decode vectors. */
typedef struct PullModuleFixture {
    XrXirTypeNode node;XrXirTypes types;XrXirType parameters[2];
    XrXirInstruction code[5];XrXirBlock blocks[3];XrXirFunction function;XrXirModule module;
} PullModuleFixture;
static void pull_module_fixture(PullModuleFixture *f,unsigned variant) {
    memset(f,0,sizeof(*f));
    bool nth=variant>=9 && variant<=17,closing=variant>=18;
    bool has=variant==0 || variant==2;
    bool unit=variant==2 || variant==3 || variant==10 || variant==19;
    bool receiver_bad=variant==4 || variant==11 || variant==20;
    XrXirType result=closing?XR_XIR_UNIT:has?XR_XIR_BOOL:unit?XR_XIR_UNIT:XR_XIR_I64;
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR,
        .element=unit?XR_XIR_UNIT:XR_XIR_I64,.result=XR_XIR_ERROR};
    f->types=(XrXirTypes){&f->node,1,NULL,NULL};
    f->parameters[0]=receiver_bad?XR_XIR_I64:(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->parameters[1]=variant==12?XR_XIR_BOOL:XR_XIR_I64;
    uint32_t parameters=nth?2u:1u;
    f->code[0]=(XrXirInstruction){.op=closing?XR_XIR_ITERATOR_CLOSE:nth?XR_XIR_ITERATOR_NTH:
        has?XR_XIR_ITERATOR_HAS_NEXT:XR_XIR_ITERATOR_NEXT,.args={0,nth?1u:0u},.targets={1,2}};
    if(variant==6 || variant==15 || variant==22)f->code[0].targets[1]=1;
    if(variant==8 || variant==17 || variant==24)f->code[0].immediate=1;
    if(variant==13)f->code[0].args[1]=2;
    if(variant==25)f->code[0].args[1]=1;
    if(variant==26)f->code[0].type=XR_XIR_I64;
    bool payload=result!=XR_XIR_UNIT || variant==21;
    uint32_t success=payload?2u:1u,error_first=1+success;
    if(payload) {
        f->code[1]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,
            .type=variant==5 || variant==14?XR_XIR_BOOL:variant==21?XR_XIR_I64:result,
            .immediate=variant==16?1:0};
        f->code[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={parameters+1}};
    } else f->code[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->code[error_first]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,
        .immediate=variant==7 || variant==23?1:0};
    f->code[error_first+1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={parameters+error_first}};
    f->blocks[0]=(XrXirBlock){.count=1};
    f->blocks[1]=(XrXirBlock){.first=1,.count=success};
    f->blocks[2]=(XrXirBlock){.first=error_first,.count=2};
    f->function=(XrXirFunction){.name="pull",.name_length=4,.parameters=f->parameters,
        .parameter_count=parameters,.result=result,.blocks=f->blocks,.block_count=3,
        .instructions=f->code,.instruction_count=error_first+2,.error_type=XR_XIR_ERROR};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=&f->function,.function_count=1,
        .types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    static const struct {const char *name;XrXirStatus expected;} cases[]={
        {"has_next_i64",XR_XIR_OK},{"next_i64",XR_XIR_OK},{"has_next_unit",XR_XIR_OK},{"next_unit",XR_XIR_OK},
        {"wrong_receiver",XR_XIR_BAD_TYPE},{"wrong_result",XR_XIR_BAD_STRUCTURE},
        {"same_successor",XR_XIR_BAD_STRUCTURE},{"wrong_origin",XR_XIR_BAD_STRUCTURE},{"unused_immediate",XR_XIR_BAD_STRUCTURE},
        {"nth_i64",XR_XIR_OK},{"nth_unit",XR_XIR_OK},{"nth_wrong_receiver",XR_XIR_BAD_TYPE},
        {"nth_wrong_index_type",XR_XIR_BAD_TYPE},{"nth_wrong_index_id",XR_XIR_BAD_VALUE},
        {"nth_wrong_result",XR_XIR_BAD_STRUCTURE},{"nth_same_successor",XR_XIR_BAD_STRUCTURE},
        {"nth_wrong_origin",XR_XIR_BAD_STRUCTURE},{"nth_unused_immediate",XR_XIR_BAD_STRUCTURE},
        {"close_i64",XR_XIR_OK},{"close_unit",XR_XIR_OK},{"close_wrong_receiver",XR_XIR_BAD_TYPE},
        {"close_result_payload",XR_XIR_BAD_STRUCTURE},{"close_same_successor",XR_XIR_BAD_STRUCTURE},
        {"close_wrong_origin",XR_XIR_BAD_STRUCTURE},{"close_unused_immediate",XR_XIR_BAD_STRUCTURE},
        {"close_unused_right",XR_XIR_BAD_STRUCTURE},{"close_wrong_result",XR_XIR_BAD_TYPE}};
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        PullModuleFixture fixture;pull_module_fixture(&fixture,n);
        XrXirArtifact *checked=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(&owner.context,&fixture.module,&checked,&diagnostic);
        if(status!=cases[n].expected)fprintf(stderr,"Pull Module %s status%u expected%u f%u i%u\n",
            cases[n].name,status,cases[n].expected,diagnostic.function,diagnostic.instruction);
        CHECK(status==cases[n].expected);
        if(status==XR_XIR_OK) {
            CHECK(checked);memset(&fixture,0xa5,sizeof(fixture));
            XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
            XrXirEffectFacts facts=xr_xir_effects_read(effects,0);bool closing=n>=18;
            CHECK(facts.status==XR_XIR_OK && facts.control.suspend==(closing?XR_XIR_EFFECT_NONE:XR_XIR_EFFECT_MAY));
            CHECK(facts.suspend_witness.cause==(closing?XR_XIR_EFFECT_CAUSE_NONE:XR_XIR_EFFECT_CAUSE_SUSPEND) &&
                !facts.suspend_witness.instruction);
            xr_xir_compile_effects_free(effects);
            CHECK(xr_xir_compile_lower(checked,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
                NULL,&lowered,&diagnostic)==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(lowered,&diagnostic)==XR_XIR_OK);
        } else CHECK(!checked);
        xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);
    }
    source_fixture_owner_free(&owner);
    puts("Current Module pull semantics: eight admitted cases, nineteen typed/CFG refusals; external packet decode not exercised");
    return 0;
}
