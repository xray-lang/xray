/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_type_receiving.c - Identity reception without carrier relabeling
 */
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "../xir_instance_compile_observer.h"
static XrXirType type(uint32_t index) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+index);}
static XrXirTypeArena *arena(SourceFixtureOwner *compiler,unsigned shift) {
    XrXirTypeNode nodes[4]={0};
    CHECK(shift<=2);
    if (shift) nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
    if (shift==2) nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL};
    nodes[shift]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    nodes[shift+1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=type(shift)};
    XrXirTypes pool={nodes,shift+2,NULL,NULL};XrXirTypeArena *result=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pool,&result)==XR_XIR_VALUE_OK);return result;
}
static void no_change(const XrXirCompileContext *context,XrCompileResourceStats before) {
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);
}
#include "type_receiving_nested.h"
#include "type_receiving_error.h"
#include "type_receiving_class.h"
#include "type_receiving_equal.h"
int main(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    received_nested_arrays(&compiler);
    received_error_arrays(&compiler);
    received_class_storage(&compiler);
    received_value_equality(&compiler);
    XrXirTypeArena *source=arena(&compiler,0),*target=arena(&compiler,1),*unrelated=arena(&compiler,1);
    XrXirTypeArena *parent=arena(&compiler,0),*child=arena(&compiler,1);
    XirTypeReceivingRoot down={type(1),type(2)},up={type(2),type(1)};
    CHECK(xir_compile_type_arena_receive(&compiler.context,child,parent,&down,1)==XR_XIR_VALUE_OK);
    xr_xir_compile_type_arena_drop(parent);
    CHECK(xir_compile_type_arena_receive(&compiler.context,parent,child,&up,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_compile_type_arena_drop(child);
    CHECK(!xir_type_arena_same_type(source,type(1),target,type(2)));
    CHECK(!xir_type_arena_same_type(target,type(2),unrelated,type(2)));
    XirTypeReceivingRoot root={type(1),type(2)},wrong={type(1),type(1)};
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(compiler.context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_compile_type_arena_receive(&compiler.context,unrelated,source,&wrong,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    no_change(&compiler.context,before);
    CHECK(!xir_type_arena_same_type(source,type(1),unrelated,type(2)));
    CHECK(xr_xir_compile_type_arena_retain(target));
    CHECK(xir_compile_type_arena_receive(&compiler.context,target,source,&root,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_compile_type_arena_drop(target);
    CHECK(xr_compile_resources_stats(compiler.context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    instance_compile_fail_at=instance_compile_attempts;
    CHECK(xir_compile_type_arena_receive(&compiler.context,target,source,&root,1)==XR_XIR_VALUE_OOM);
    instance_compile_fail_at=SIZE_MAX;
    no_change(&compiler.context,before);
    CHECK(!xir_type_arena_same_type(source,type(1),target,type(2)));
    CHECK(xir_compile_type_arena_receive(&compiler.context,target,source,&root,1)==XR_XIR_VALUE_OK);
    CHECK(xir_type_arena_same_type(source,type(1),target,type(2)));
    CHECK(xir_type_arena_same_type(source,type(0),target,type(1)));
    CHECK(!xir_type_arena_same_type(source,type(0),target,type(0)));
    CHECK(!xir_type_arena_same_type(source,type(1),unrelated,type(2)));
    XrXirTypeArena *third=arena(&compiler,2);XirTypeReceivingRoot chained={type(2),type(3)};
    CHECK(xir_compile_type_arena_receive(&compiler.context,third,target,&chained,1)==XR_XIR_VALUE_OK);
    CHECK(xir_type_arena_same_type(source,type(1),third,type(3)));
    XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1024*1024,&foreign)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue values[]={{XR_XIR_I64,0,41},{XR_XIR_I64,0,42}},array={0},cell={0};
    CHECK(xr_xir_array_new(type(0),values,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,source,type(1),&array,&admission,&cell)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_argument(&cell,target,type(2)) && xr_xir_value_argument(&cell,third,type(3)));
    CHECK(!xr_xir_value_argument(&cell,unrelated,type(2)));
    admission.arena=target;
    CHECK(xr_xir_value_admit(&cell,type(2),&admission)==XR_XIR_VALUE_OK);
    int64_t length=-731;XrXirFaultDetail fault={0};XrXirValue element={0},reserved={0};
    CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_OK && length==2);
    CHECK(xr_xir_array_capacity(&array,&admission,&length)==XR_XIR_VALUE_OK && length>=2);
    CHECK(xr_xir_array_get(&array,1,&admission,&element,&fault)==XR_XIR_VALUE_OK && element.payload==42);
    xr_xir_value_drop(&element);
    CHECK(xr_xir_array_reserve(&array,8,&admission,&reserved)==XR_XIR_VALUE_OK);
    CHECK(reserved.type==array.type && xr_xir_value_arena(&reserved)==source && reserved.payload!=array.payload);
    XrXirValuePlace array_place={type(1),&reserved.payload};values[0].payload=63;
    CHECK(xr_xir_array_set(&array_place,0,&values[0],&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_push(&array_place,&values[0],&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_len(&reserved,&admission,&length)==XR_XIR_VALUE_OK && length==3);
    CHECK(xr_xir_array_get(&reserved,0,&admission,&element,&fault)==XR_XIR_VALUE_OK && element.payload==63);
    xr_xir_value_drop(&element);xr_xir_value_drop(&reserved);
    uint64_t saved_work=admission.work;admission.work=0;length=-731;
    CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_LIMIT && length==-731);
    admission.work=saved_work;admission.arena=unrelated;
    CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-731);
    admission.arena=target;
    CHECK(cell.type==(uint32_t)type(1) && xr_xir_value_arena(&cell)==source);
    XrXirValue recovered={0};
    CHECK(xr_xir_value_from_payload(target,type(2),cell.payload,&recovered) && recovered.type==cell.type && recovered.payload==cell.payload);
    XrXirValue sentinel={XR_XIR_I64,0,731};recovered=sentinel;
    CHECK(!xr_xir_value_from_payload(unrelated,type(2),cell.payload,&recovered) && !memcmp(&recovered,&sentinel,sizeof(recovered)));
    XrXirType mapped=XR_XIR_BOOL;
    CHECK(xir_type_arena_local_type(target,source,type(1),&mapped) && mapped==type(2));
    CHECK(!xir_type_arena_local_type(unrelated,source,type(1),&mapped) && mapped==type(2));
    XrXirValue replacement={0};values[0].payload=51;values[1].payload=52;
    CHECK(xr_xir_array_new(type(1),values,2,&admission,&replacement)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&cell,&replacement,&admission)==XR_XIR_VALUE_OK && xr_xir_value_valid(&cell));
    XrXirValue read={0};CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK);
    CHECK(read.type==replacement.type && read.payload==replacement.payload && xr_xir_value_arena(&read)==target);
    CHECK(xr_xir_value_admit(&cell,type(2),&admission)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&read);xr_xir_value_drop(&replacement);
    admission.domain=foreign;
    CHECK(xr_xir_value_admit(&cell,type(2),&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_compile_type_arena_drop(source);xr_xir_compile_type_arena_drop(target);
    CHECK(xr_xir_value_argument(&cell,third,type(3)));
    xr_xir_value_drop(&cell);xr_xir_value_drop(&array);
    xr_xir_domain_drop(foreign);xr_xir_domain_drop(domain);
    xr_xir_compile_type_arena_drop(unrelated);xr_xir_compile_type_arena_drop(third);
    source_fixture_owner_free(&compiler);instance_compile_zero();
    puts("Actual nested Cell/Array carrier retained its birth type through two receiving identities; unrelated lookalike, foreign Domain and failed preparation rejected; physical0");
    return 0;
}
