/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_structural_wire.c - Complete independent declaration-bound field packets
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "../xir_structural81_golden.h"
#include "../composed_wire/structural90.inc.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_source_fixture_owner.h"
#include "../composed_wire/retired.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA==34 && XR_XIR_CHECKED_CONTRACT==90 &&
    XR_XIR_VALUE_ABI_VERSION==28 && XR_XIR_PROGRAM_ABI_VERSION==37,"structural packet identity");
_Static_assert(XR_XIR_FIELD_REQUIREMENT==186,"definition-bound field operation");
static void structural_wire_retired(SourceFixtureOwner *owner) {
    const ComposedRetiredPacket packets[]={
        {structural81_relocated,sizeof(structural81_relocated)},
        {structural81_policy_zero,sizeof(structural81_policy_zero)},
        {structural81_policy_unknown,sizeof(structural81_policy_unknown)},
        {structural81_tuple_shape,sizeof(structural81_tuple_shape)},
        {structural81_extra_selection,sizeof(structural81_extra_selection)},
        {structural81_result_mismatch,sizeof(structural81_result_mismatch)},
        {structural81_missing,sizeof(structural81_missing)},
        {structural81_field_mismatch,sizeof(structural81_field_mismatch)},
        {structural81_unused_policy,sizeof(structural81_unused_policy)},
        {structural81_unused_extra,sizeof(structural81_unused_extra)},
        {structural81_outside_binder,sizeof(structural81_outside_binder)},
        {structural81_conflict,sizeof(structural81_conflict)}
    };
    composed_retired_reject(&owner->context,packets,sizeof(packets)/sizeof(*packets));
}
static XrXirArtifact *structural_lower(SourceFixtureOwner *owner) {
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    size_t length=sizeof(structural90_relocated);void *input=NULL;
    CHECK(xr_compile_resources_alloc(owner->context.resources,length,&input)==XR_COMPILE_RESOURCE_OK);
    memcpy(input,structural90_relocated,length);
    XrXirStatus status=xr_xir_compile_checked_read(&owner->context,input,length,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"structural read=%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    memset(input,0xa5,length);xr_compile_resources_free(input);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    CHECK(module->generics[2].parameter_count==1);
    const XrXirConstraint *constraint=&module->generics[2].constraints[0];
    CHECK(constraint->structural_count==1 && constraint->structural_requirements[0].shape==257 &&
        constraint->structural_requirements[0].policy==XR_XIR_STRUCTURAL_READONLY_RECEIVER);
    CHECK(module->functions[2].instructions[0].op==XR_XIR_FIELD_REQUIREMENT &&
        module->functions[2].instructions[0].immediate==0);
    XrXirCheckedPacket encoded={0};CHECK(xr_xir_compile_checked_write(checked,&encoded,NULL)==XR_XIR_OK);
    CHECK(encoded.length==length && !memcmp(encoded.bytes,structural90_relocated,length));
    xr_xir_compile_checked_packet_free(&encoded);
    status=xr_xir_compile_specialize(checked,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"structural specialize=%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && closed);xr_xir_compile_artifact_free(checked);
    module=xr_xir_compile_artifact_module(closed);uint32_t projections=0;
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
            const XrXirInstruction *op=&module->functions[f].instructions[i];
            CHECK(op->op!=XR_XIR_FIELD_REQUIREMENT);
            if (op->op==XR_XIR_RECORD_FIELD) {CHECK(op->immediate==1 && op->type==XR_XIR_I64);++projections;}
        }
    CHECK(projections==1);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,&diagnostic)==XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(closed);return lowered;
}
static void structural_run(XrXirArtifact *lowered) {
    XrXirProgram *program=NULL;XrXirInstance *instance=NULL;XrXirInstanceConfig config;XrXirValue result={0};
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(result.type==XR_XIR_I64 && result.payload==7);xr_xir_value_drop(&result);
}
static void structural_reject(SourceFixtureOwner *owner) {
    const struct {const char *name;const uint8_t *bytes;size_t length;} cases[]={
#define BAD(name) {#name,name,sizeof(name)}
        BAD(structural90_policy_zero),BAD(structural90_policy_unknown),BAD(structural90_tuple_shape),
        BAD(structural90_extra_selection),BAD(structural90_result_mismatch),BAD(structural90_missing),
        BAD(structural90_field_mismatch),BAD(structural90_unused_policy),BAD(structural90_unused_extra),
        BAD(structural90_outside_binder),BAD(structural90_conflict)
#undef BAD
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirArtifact *output=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_checked_read(&owner->context,cases[i].bytes,cases[i].length,&output,&diagnostic);
        printf("%s status=%u f%u i%u\n",cases[i].name,status,diagnostic.function,diagnostic.instruction);
        CHECK(status==XR_XIR_BAD_TYPE && !output);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==3 && !strcmp(argv[1],"--emit")));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    structural_wire_retired(&owner);
    XrXirArtifact *lowered=structural_lower(&owner);
    if (argc==3) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"language_reference",1048576,&source)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    } else structural_reject(&owner);
    structural_run(lowered);source_fixture_owner_free(&owner);
    puts("Independent structural fields, declaration rejection and compiler ledger restored PASS");return 0;
}
