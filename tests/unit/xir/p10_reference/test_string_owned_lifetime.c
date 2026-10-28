/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_owned_lifetime.c - Owned UTF-8 results after execution owners drop
 */
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "../xir_instance_compile_observer.h"
#include "../xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec language_reference_program;

static uint32_t find_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
static bool selected_vm(unsigned mode,uint32_t function,const uint32_t roots[3]) {
    bool root=function==roots[0] || function==roots[1] || function==roots[2];
    return mode==0 || (mode==2 && root) || (mode==3 && !root);
}
static void late_results(XrXirValue results[3]) {
    static const unsigned char text[]={240,159,153,130,0,240,159,153,130};
    static const unsigned char bytes[]={65,0,195,169,240,159,153,130};
    const char *view=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&results[1],&view,&length) && length==sizeof(text));
    CHECK(!memcmp(view,text,sizeof(text)));
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(1048576,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(&results[2]),reader,NULL,NULL,100000,1048576};
    int64_t count=-1;CHECK(xr_xir_array_len(&results[2],&admission,&count)==XR_XIR_VALUE_OK && count==8);
    for(int64_t i=0;i<count;++i) {
        XrXirValue item={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(&results[2],i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(item.type==XR_XIR_U8 && item.payload==bytes[i]);xr_xir_value_drop(&item);
    }
    CHECK(results[0].type==XR_XIR_I64 && results[0].payload==42);
    xr_xir_domain_drop(reader);
    for(unsigned i=0;i<3;++i) xr_xir_value_drop(&results[i]);
}
static void execute(unsigned mode) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgramSpec spec=language_reference_program;XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,spec.packet.bytes,spec.packet.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&spec.target, NULL,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t roots[]={find_function(module,"referenceAnswer"),find_function(module,"retainedText"),find_function(module,"retainedBytes")};
    XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
    CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*entries),(void **)&entries)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    uint32_t vm=0,native=0,crossing=0;
    for(uint32_t f=0;f<spec.entry_count;++f) {
        CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
        bool use_vm=selected_vm(mode,f,roots);
        if(use_vm) {CHECK(entries[f].resume!=spec.entries[f].resume);++vm;}
        else {entries[f]=spec.entries[f];++native;}
        const XrXirFunction *function=&module->functions[f];
        for(uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirInstruction *instruction=&function->instructions[i];
            if(instruction->op==XR_XIR_CALL) {
                CHECK(instruction->immediate<spec.entry_count);
                crossing+=use_vm!=selected_vm(mode,(uint32_t)instruction->immediate,roots);
            }
        }
    }
    if(mode>=2) CHECK(vm && native && crossing);
    spec.entries=entries;spec.types=module->types;spec.declarations=module->declarations;
    XrXirProgramPacket packet=xir_program_packet_borrow(lowered);
    CHECK(packet.identity && packet.length==spec.packet.length && !memcmp(packet.identity,spec.packet.identity,32));
    spec.packet=packet;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    XrXirValue results[3]={{0}};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(unsigned i=0;i<3;++i) {
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,roots[i],NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&results[i])==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);
    xr_compile_resources_free(bindings);xr_compile_resources_free(entries);
    late_results(results);source_fixture_owner_free(&owner);instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes);
    printf("owned UTF-8 results after Instance/Program/artifact drop: mode=%u vm=%u native=%u crossing=%u\n",mode,vm,native,crossing);
}
int main(int argc,char **argv) {
    CHECK(argc==2 && argv[1][0]>='0' && argv[1][0]<='3' && !argv[1][1]);
    execute((unsigned)(argv[1][0]-'0'));return 0;
}
