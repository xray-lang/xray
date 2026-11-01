/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_fixed_bytes.c - Exact byte payload and scalar length ownership
 *
 * KEY CONCEPT:
 *   The byte count is type metadata; inline storage contains bytes, not handles.
 */
#include "xir/xxir_fixed_array.h"
#include "xir/xxir_array.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fixed %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_fixture_owner.h"
#include "xir_construction_fixture.h"
XR_FUNC void fixed_bytes_fail_next(void);
XR_FUNC bool fixed_bytes_failure_observed(void);
static void fixed_shapes(const XrXirCompileContext *context) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=XR_XIR_U8,.parameter_count=4},
        {.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=XR_XIR_U8,.parameter_count=5}};
    XrXirTypes types={nodes,2,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(!xr_xir_type_parameter_slots(nodes));
    XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    const XrXirTypes *owned=xr_xir_compile_type_arena_types(arena);
    CHECK(owned!=&types && owned->nodes!=nodes && !owned->nodes[0].parameters &&
        owned->nodes[0].parameter_count==4 && owned->nodes[1].parameter_count==5);
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    const XrXirStorageLayout *storage=xr_xir_compile_type_arena_storage(arena,type);
    CHECK(storage && storage->value.size==4 && storage->value.alignment==1 && !storage->owned_depth);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};XrXirLayout layout={0};
    CHECK(xr_xir_compile_layout(context,&types,type,&target,XR_XIR_LAYOUT_FRAME,&layout)==XR_XIR_OK && layout.size==8);
    xr_xir_compile_type_arena_drop(arena);
    const XrXirTypeNode original=nodes[0];
    nodes[0].parameter_count=0;CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=original;nodes[0].parameter_count=UINT32_MAX;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_layout(context,&types,type,&target,XR_XIR_LAYOUT_STORAGE,&layout)==XR_XIR_OK &&
        layout.size==UINT32_MAX && layout.alignment==1);
    nodes[0].parameter_count=65535;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    nodes[0].parameter_count=65536;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=original;nodes[0].element=XR_XIR_I64;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    XrXirCallableParameter fake={XR_XIR_U8,0};nodes[0]=original;nodes[0].parameters=&fake;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=original;nodes[0].parameter_span=1;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=original;nodes[1]=original;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_STRUCTURE);
}
static void fixed_values(const XrXirCompileContext *context) {
    const XrXirType fixed=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    const XrXirType array=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=XR_XIR_U8,.parameter_count=12},
        {.kind=XR_XIR_TYPE_ARRAY,.element=fixed}};
    const XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    const XrXirDomainStats baseline=xr_xir_domain_stats(domain);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,1048576};
    const uint8_t golden[]={0,255,137,80,78,71,3,4,5,6,7,8};
    XrXirValue value={0},copy={0},items={0},roundtrip={0},out={0};int64_t length=-1;
    fixed_bytes_fail_next();
    CHECK(xr_xir_fixed_array_from_bytes(fixed,golden,12,&admission,&value)==XR_XIR_VALUE_OOM &&
        fixed_bytes_failure_observed() && !value.type && !value.payload &&
        xr_xir_domain_stats(domain).live_bytes==baseline.live_bytes);
    CHECK(xr_xir_fixed_array_from_bytes(fixed,golden,12,&admission,&value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_valid(&value) && xr_xir_fixed_array_count(&value,&length)==XR_XIR_VALUE_OK && length==12);
    CHECK(xr_xir_value_copy(&value,&copy)==XR_XIR_VALUE_OK && value.payload==copy.payload);
    CHECK(xr_xir_array_new(array,&value,1,&admission,&items)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&value);xr_xir_value_drop(&copy);
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&items,0,&admission,&roundtrip,&fault)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&items);
    for(uint32_t i=0;i<12;++i) {
        CHECK(xr_xir_fixed_array_get(&roundtrip,i,&out,&fault)==XR_XIR_VALUE_OK && out.type==XR_XIR_U8 && out.payload==golden[i]);
        xr_xir_value_drop(&out);
    }
    CHECK(xr_xir_fixed_array_get(&roundtrip,-1,&out,&fault)==XR_XIR_VALUE_BOUNDS && !out.type &&
        xr_xir_fault_bounds_valid(fault) && fault.index==-1 && fault.length==12);
    CHECK(xr_xir_fixed_array_get(&roundtrip,12,&out,&fault)==XR_XIR_VALUE_BOUNDS && !out.type && fault.index==12);
    XrXirValue scalar={XR_XIR_I64,0,3};
    CHECK(xr_xir_fixed_array_get(&scalar,0,&out,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    length=91;CHECK(xr_xir_fixed_array_count(&scalar,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==91);
    CHECK(xr_xir_fixed_array_from_bytes(fixed,golden,11,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    CHECK(xr_xir_fixed_array_from_bytes(fixed,NULL,12,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    XrXirValue elements[12];for(uint32_t i=0;i<12;++i)elements[i]=(XrXirValue){XR_XIR_U8,0,golden[i]};
    elements[4].type=XR_XIR_I64;
    CHECK(xr_xir_fixed_array_new(fixed,elements,12,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    elements[4].type=XR_XIR_U8;admission.work=11;
    CHECK(xr_xir_fixed_array_new(fixed,elements,12,&admission,&out)==XR_XIR_VALUE_LIMIT && !out.type);
    admission.work=1000000;CHECK(xr_xir_fixed_array_new(fixed,elements,12,&admission,&out)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&roundtrip);xr_xir_value_drop(&out);
    CHECK(xr_xir_domain_stats(domain).live_bytes==baseline.live_bytes);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
}
static void fixed_instructions(const XrXirCompileContext *context) {
    XrXirType fixed=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=XR_XIR_U8,.parameter_count=2};
    XrXirTypes types={&node,1,NULL,NULL};uint32_t operands[]={0,1};
    XrXirInstruction code[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_U8,.immediate=137},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_U8,.immediate=80},
        {.op=XR_XIR_FIXED_ARRAY_NEW,.type=fixed,.args={0,2}},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_FIXED_ARRAY_GET,.type=XR_XIR_U8,.args={2,3}},
        {.op=XR_XIR_FIXED_ARRAY_LEN,.type=XR_XIR_I64,.args={2}},
        {.op=XR_XIR_RETURN,.args={5}}};
    XrXirBlock block={.count=7};
    XrXirFunction function={.name="fixed",.name_length=5,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=code,.instruction_count=7,
        .operands=operands,.operand_count=2,.error_type=XR_XIR_NEVER};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,
        .types=&types,.linkage_kind=XR_XIR_PROGRAM};
    for(unsigned attack=0;attack<8;++attack) {
        XrXirInstruction saved[7];memcpy(saved,code,sizeof(code));
        if(attack==1)code[2].args[1]=1;
        if(attack==2)code[0].type=XR_XIR_I64;
        if(attack==3)code[4].type=XR_XIR_I64;
        if(attack==4)code[3].type=XR_XIR_U8;
        if(attack==5)code[4].args[0]=0;
        if(attack==6)code[5].type=XR_XIR_U8;
        if(attack==7)code[2].immediate=1;
        XrXirArtifact *output=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(context,&module,&output,&diagnostic);
        XrXirStatus expected=!attack?XR_XIR_OK:
            (attack==1 || attack==7)?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        if(status!=expected)fprintf(stderr,"fixed typed attack%u status%u expected%u f%u/i%u\n",
            attack,status,expected,diagnostic.function,diagnostic.instruction);
        CHECK(status==expected && (output!=NULL)==!attack);
        xr_xir_compile_artifact_free(output);memcpy(code,saved,sizeof(code));
    }
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    fixed_shapes(&owner.context);fixed_values(&owner.context);fixed_instructions(&owner.context);
    source_fixture_owner_free(&owner);
    puts("fixed bytes: scalar lengths, exact storage, retirement and refusals");return 0;
}
