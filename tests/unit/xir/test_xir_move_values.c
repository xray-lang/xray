/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_move_values.c - Whole local ownership transfer and reinitialization
 *
 * KEY CONCEPT:
 *   A take preserves payload identity without allocating or retaining it.
 *   The empty Cell remains destructible but grants no read or loan.
 */
#include "xir/xxir_value_take_internal.h"
#include "xir/xxir_cell_owner_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"move %d: %s\n",__LINE__,#c);exit(1);}}while(0)

static void new_take_controls(XrXirTypeArena *arena,XrXirDomain *domain,XrXirValueAdmission *admission) {
    XrXirType array=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType cell_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    const XrXirValue element={XR_XIR_I64,0,42};XrXirValue initial={0},cell={0};
    CHECK(xr_xir_array_new(array,&element,1,admission,&initial)==XR_XIR_VALUE_OK);
    int64_t word=0;xr_xir_owned_slot_move(&word,0,&initial);
    XrXirValue borrowed=xr_xir_value_load(arena,array,word);int64_t identity=word;
    XirCellNewTake prepared={0};
    CHECK(xir_cell_new_take_prepare(cell_type,&borrowed,NULL,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    CHECK(xir_cell_new_take_prepare(XR_XIR_I64,&borrowed,admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    CHECK(xir_cell_new_take_prepare(cell_type,&element,admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    XrXirDomain *foreign=NULL;CHECK(xr_xir_domain_new(UINT64_C(1048576),&foreign)==XR_XIR_VALUE_OK);
    XrXirValueAdmission other=*admission;other.domain=foreign;
    CHECK(xir_cell_new_take_prepare(cell_type,&borrowed,&other,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    xr_xir_domain_drop(foreign);CHECK(word==identity&&xr_xir_value_valid(&borrowed));
    XrXirDomainStats frame=xr_xir_domain_stats(domain);
    CHECK(xir_cell_new_take_prepare(cell_type,&borrowed,admission,&prepared)==XR_XIR_VALUE_OK);
    xir_cell_new_take_abort(&prepared);
    CHECK(!prepared.cell&&word==identity&&xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    CHECK(xir_cell_new_take_prepare(cell_type,&borrowed,admission,&prepared)==XR_XIR_VALUE_OK);
    xir_cell_new_take_commit(&prepared,&word,&cell);
    CHECK(!prepared.cell&&!word&&xr_xir_value_valid(&cell));borrowed=(XrXirValue){0};
    XirCellTake taken={0};CHECK(xir_cell_take_prepare(&cell,array,admission,&taken)==XR_XIR_VALUE_OK);
    XrXirValue result={0};xir_cell_take_commit(&taken,&result);
    CHECK(result.payload==identity);xr_xir_value_drop(&cell);xr_xir_value_drop(&result);
}
static void write_take_controls(XrXirTypeArena *arena,XrXirDomain *domain,XrXirValueAdmission *admission) {
    XrXirType array=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType cell_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    const XrXirValue item={XR_XIR_I64,0,42};XrXirValue initial={0},cell={0},source={0},read={0};
    CHECK(xr_xir_array_new(array,&item,1,admission,&initial)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,cell_type,&initial,admission,&cell)==XR_XIR_VALUE_OK);
    int64_t previous=initial.payload,cell_identity=cell.payload;xr_xir_value_drop(&initial);
    CHECK(xr_xir_array_new(array,&item,1,admission,&source)==XR_XIR_VALUE_OK);
    int64_t word=0;xr_xir_owned_slot_move(&word,0,&source);
    XrXirValue borrowed=xr_xir_value_load(arena,array,word);int64_t next=word;
    XirCellWriteTake prepared={0};const XrXirCellAuthority authority={0};
    CHECK(xir_cell_write_take_prepare(&cell,&borrowed,&authority,NULL,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    CHECK(xir_cell_write_take_prepare(&cell,&item,&authority,admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    XrXirValueAdmission limited=*admission;limited.work=0;
    CHECK(xir_cell_write_take_prepare(&cell,&borrowed,&authority,&limited,&prepared)==XR_XIR_VALUE_LIMIT&&!prepared.cell);
    XrXirDomain *foreign=NULL;CHECK(xr_xir_domain_new(UINT64_C(1048576),&foreign)==XR_XIR_VALUE_OK);
    XrXirValueAdmission other=*admission;other.domain=foreign;
    CHECK(xir_cell_write_take_prepare(&cell,&borrowed,&authority,&other,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    xr_xir_domain_drop(foreign);
    XrXirCellLoan loan={0};CHECK(xr_xir_cell_loan_prepare(&cell,NULL,&loan)==XR_XIR_VALUE_OK);
    CHECK(xir_cell_write_take_prepare(&cell,&borrowed,&authority,admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    xr_xir_cell_loan_abort(&loan);
    XrXirDomainStats frame=xr_xir_domain_stats(domain);
    CHECK(xir_cell_write_take_prepare(&cell,&borrowed,&authority,admission,&prepared)==XR_XIR_VALUE_OK);
    xir_cell_write_take_abort(&prepared);
    CHECK(!prepared.cell&&word==next&&xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK&&read.payload==previous);xr_xir_value_drop(&read);
    CHECK(xir_cell_write_take_prepare(&cell,&borrowed,&authority,admission,&prepared)==XR_XIR_VALUE_OK);
    xir_cell_write_take_commit(&prepared,&word);
    CHECK(!prepared.cell&&!word&&cell.payload==cell_identity);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK&&read.payload==next);
    xr_xir_value_drop(&read);xr_xir_value_drop(&cell);
}
static void unit_take_controls(XrXirDomain *domain,XrXirValueAdmission *admission) {
    XrXirType cell_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2);
    XrXirValue unit={0},cell={0},taken={0};XirCellNewTake allocation={0};
    CHECK(xir_cell_new_take_prepare(cell_type,&unit,admission,&allocation)==XR_XIR_VALUE_OK);
    xir_cell_new_take_commit(&allocation,NULL,&cell);
    CHECK(xr_xir_value_valid(&cell));XirCellTake transfer={0};
    CHECK(xir_cell_take_prepare(&cell,XR_XIR_UNIT,admission,&transfer)==XR_XIR_VALUE_OK);
    xir_cell_take_commit(&transfer,&taken);CHECK(!taken.type&&!taken.payload&&xr_xir_value_valid(&cell));
    XirCellWriteTake replacement={0};const XrXirCellAuthority authority={0};
    CHECK(xir_cell_write_take_prepare(&cell,&unit,&authority,admission,&replacement)==XR_XIR_VALUE_OK);
    xir_cell_write_take_commit(&replacement,NULL);
    CHECK(xr_xir_cell_read(&cell,&taken)==XR_XIR_VALUE_OK&&!taken.type&&!taken.payload);
    CHECK(xr_xir_domain_stats(domain).live_bytes>0);xr_xir_value_drop(&cell);
}
int main(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirType array=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType cell_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=array},{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT}};
    const XrXirTypes types={nodes,3,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_C(1048576),&domain)==XR_XIR_VALUE_OK);
    XrXirDomainStats frame=xr_xir_domain_stats(domain);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX};
    new_take_controls(arena,domain,&admission);
    write_take_controls(arena,domain,&admission);
    unit_take_controls(domain,&admission);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    XrXirValue initial={0},cell={0},taken={0},read={0};
    const XrXirValue elements[]={{XR_XIR_I64,0,20},{XR_XIR_I64,0,22}};
    CHECK(xr_xir_array_new(array,elements,2,&admission,&initial)==XR_XIR_VALUE_OK);
    int64_t identity=initial.payload;
    CHECK(xr_xir_cell_new(domain,arena,cell_type,&initial,&admission,&cell)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&initial);
    XirCellTake prepared={0};
    CHECK(xir_cell_take_prepare(&cell,array,NULL,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    XrXirValueAdmission exhausted=admission;exhausted.work=0;
    CHECK(xir_cell_take_prepare(&cell,array,&exhausted,&prepared)==XR_XIR_VALUE_LIMIT&&!prepared.cell);
    CHECK(xir_cell_take_prepare(&cell,XR_XIR_STRING,&admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    XrXirCellLoan loan={0};
    CHECK(xr_xir_cell_loan_prepare(&cell,NULL,&loan)==XR_XIR_VALUE_OK);
    CHECK(xir_cell_take_prepare(&cell,array,&admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    xr_xir_cell_loan_abort(&loan);
    CHECK(xir_cell_take_prepare(&cell,array,&admission,&prepared)==XR_XIR_VALUE_OK);
    prepared=(XirCellTake){0};
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK&&read.payload==identity);
    xr_xir_value_drop(&read);
    XrXirDomainStats current=xr_xir_domain_stats(domain);
    CHECK(xir_cell_take_prepare(&cell,array,&admission,&prepared)==XR_XIR_VALUE_OK);
    xir_cell_take_commit(&prepared,&taken);
    CHECK(!prepared.cell&&taken.payload==identity&&xr_xir_value_valid(&cell));
    CHECK(xr_xir_domain_stats(domain).allocations==current.allocations&&
        xr_xir_domain_stats(domain).live_bytes==current.live_bytes);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_BAD_ARGUMENT&&!read.type);
    CHECK(xir_cell_take_prepare(&cell,array,&admission,&prepared)==XR_XIR_VALUE_BAD_ARGUMENT&&!prepared.cell);
    CHECK(xr_xir_cell_loan_prepare(&cell,NULL,&loan)==XR_XIR_VALUE_BAD_ARGUMENT&&!loan.cell);
    const XrXirCellPublication publication={&context,domain,0};
    CHECK(xr_xir_cell_publication_prepare(&cell,&publication)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_array_new(array,elements,1,&admission,&initial)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&cell,&initial,&admission)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&initial);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK);int64_t length=0;
    CHECK(xr_xir_array_len(&read,&admission,&length)==XR_XIR_VALUE_OK&&length==1);
    xr_xir_value_drop(&read);
    CHECK(xir_cell_take_prepare(&cell,array,&admission,&prepared)==XR_XIR_VALUE_OK);
    XrXirValue second={0};xir_cell_take_commit(&prepared,&second);
    xr_xir_value_drop(&cell);xr_xir_value_drop(&taken);xr_xir_value_drop(&second);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);
    printf("Function declaration size: %zu bytes\n",sizeof(XrXirFunction));
    puts("Whole local take preserves payload identity, rejects loans and restores owners");return 0;
}
