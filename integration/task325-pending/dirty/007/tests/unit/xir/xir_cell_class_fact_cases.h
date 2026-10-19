/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_class_fact_cases.h - Actual Class subjects on one Cell proof graph
 */
#ifndef XIR_CELL_CLASS_FACT_CASES_H
#define XIR_CELL_CLASS_FACT_CASES_H

typedef struct CellClassFactFixture {
    XrXirNominalField fields[2];XrXirNominalDeclaration nominals[2];
    XrXirNominalTable nominal_table;XrXirType field_types[2],i64,cell,object;
    XrXirTypeNode nodes[5];XrXirTypes types;
    XrXirInstruction init[5],constructor[2],read[2],formal[5],entry[16];
    uint32_t constructor_operands[2],initial_operand,formal_operand,entry_operands[6];
    XrXirBlock initial,constructor_block,read_block,formal_block,entry_blocks[4];
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} CellClassFactFixture;

/* These expected subjects are literal SSA identities, not outputs selected
 * from the provider's Class table. The Program entry really returns I64. */
static void cell_class_fact_fixture(CellClassFactFixture *f,unsigned mode) {
    CHECK(mode<6);memset(f,0,sizeof(*f));
    f->i64=XR_XIR_I64;f->cell=(XrXirType)257;f->object=(XrXirType)256;
    f->fields[0]=(XrXirNominalField){{"first",5},XR_XIR_I64,XR_XIR_FIELD_MUTABLE};
    f->fields[1]=(XrXirNominalField){{"second",6},XR_XIR_I64,XR_XIR_FIELD_MUTABLE};
    f->nominals[0]=(XrXirNominalDeclaration){.module={"root",4},.name={"Subject",7},
        .exported=1,.fields=f->fields,.field_count=2,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals[1]=(XrXirNominalDeclaration){.module={"root",4},.name={"Pair",4},
        .exported=1,.fields=f->fields,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    f->nominal_table=(XrXirNominalTable){f->nominals,2,NULL};
    f->field_types[0]=f->field_types[1]=XR_XIR_I64;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,f->field_types,2}};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,f->field_types,1}};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)258};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256};
    f->types=(XrXirTypes){f->nodes,5,&f->nominal_table,NULL};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};f->initial=(XrXirBlock){0,1,0,0};
    f->constructor[0]=(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=f->object,.args={0,2}};
    f->constructor[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->constructor_block=(XrXirBlock){0,2,0,0};
    f->read[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->read[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};f->read_block=(XrXirBlock){0,2,0,0};
    f->formal[0]=(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=f->object};
    f->formal[1]=(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=XR_XIR_I64,.args={1}};
    f->formal[2]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=f->cell,.args={2}};
    f->formal[3]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=2};
    f->formal[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4}};
    f->formal_operand=3;f->formal_block=(XrXirBlock){0,5,0,0};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=f->object,.args={0,1},.immediate=1};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=f->object,.args={1}};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=XR_XIR_I64,.args={2}};
    f->entry[4]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=f->cell,.args={3}};
    f->entry[5]=(XrXirInstruction){.op=XR_XIR_COPY,.type=f->cell,.args={4}};
    f->entry[6]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={1,1},.immediate=2};
    f->entry[7]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={6}};
    f->entry_operands[1]=5;uint32_t count=8,operands=2,blocks=1;
    if (mode==1) {
        f->slot=(XrXirSlot){0,(XrXirType)260,1};
        f->init[0]=f->entry[0];f->init[1]=f->entry[1];
        f->init[2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)260,.args={1}};
        f->init[3]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={2}};
        f->init[4]=(XrXirInstruction){.op=XR_XIR_RETURN};f->initial.count=5;
        f->entry[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)260};
        f->entry[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=f->object};
        f->entry[6].args[0]=0;f->entry_operands[0]=5;operands=1;
    } else if (mode==2 || mode==3) {
        f->entry[2]=f->entry[1];f->entry[2].args[0]=1;
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=f->object,.args={1}};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=XR_XIR_I64,.args={3}};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=f->cell,.args={4}};
        f->entry[6]=(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=f->object,.args={mode==2?2u:1u}};
        f->entry[7]=(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=XR_XIR_I64,.args={6},.immediate=1};
        f->entry[8]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=f->cell,.args={7}};
        f->entry[9]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
        f->entry[10]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={9},.targets={1,2}};
        f->entry[11]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};f->entry[12]=f->entry[11];
        f->entry[13]=(XrXirInstruction){.op=XR_XIR_PHI,.type=f->cell,.args={2,4}};
        f->entry[14]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={13}};
        f->entry[15]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={14}};
        const uint32_t pairs[]={0,0,1,5,2,8};memcpy(f->entry_operands,pairs,sizeof(pairs));
        f->entry_blocks[0]=(XrXirBlock){0,11,0,0};f->entry_blocks[1]=(XrXirBlock){11,1,0,0};
        f->entry_blocks[2]=(XrXirBlock){12,1,0,0};f->entry_blocks[3]=(XrXirBlock){13,3,0,0};
        count=16;operands=6;blocks=4;
    } else if (mode==4) {
        f->entry[1]=(XrXirInstruction){.op=XR_XIR_STRUCT_NEW,.type=(XrXirType)258,.args={0,1}};
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)259,.args={1}};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_CELL_PLACE,.type=(XrXirType)258,.args={2}};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=XR_XIR_I64,.args={3}};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=f->cell,.args={4}};
        f->entry[6]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={5}};operands=1;
    } else if (mode==5) {
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_PLACE_READ,.type=XR_XIR_I64,.args={3}};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4}};count=6;operands=1;
    }
    if (blocks==1) f->entry_blocks[0]=(XrXirBlock){0,count,0,0};
    f->functions[0]=(XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&f->initial,1,f->init,f->initial.count,
        mode==1?&f->initial_operand:NULL,mode==1?1u:0u};
    f->functions[1]=(XrXirFunction){"new",3,&f->i64,1,f->object,&f->constructor_block,1,
        f->constructor,2,f->constructor_operands,2};
    f->functions[2]=(XrXirFunction){"read",4,&f->cell,1,XR_XIR_I64,&f->read_block,1,f->read,2,NULL,0};
    f->functions[3]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,f->entry_blocks,blocks,f->entry,count,
        f->entry_operands,operands};
    f->functions[4]=(XrXirFunction){"formal",6,&f->object,1,XR_XIR_I64,&f->formal_block,1,
        f->formal,5,&f->formal_operand,1};
    f->identities[1]=(XrXirFunctionIdentity){.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR};
    f->source=(XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.slots=mode==1?&f->slot:NULL,.slot_count=mode==1?1u:0u,.entry_function=3};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,5,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
}

static XrXirStatus cell_class_fact_view(const XrXirCompileContext *context,
    XrXirCellProvenance *proof,unsigned mode) {
    const uint32_t selected[]={5,5,13,13,5,3};
    XrXirCellClassOriginView view={0};
    XrXirStatus status=xr_xir_compile_cell_class_origin_view(context,proof,3,selected[mode],&view);
    if (status!=XR_XIR_OK) return status;
    CHECK(!view.residual_intrinsic_mask && !view.cells.parameter_count);
    if (mode<4) {
        CHECK(view.cells.intrinsic_mask==XR_XIR_CELL_ACCESS_UNKNOWN && view.word_count==1 &&
            view.root_count==(mode<2?1u:2u) && view.dependencies[0]==(mode<2?1u:3u));
        CHECK(view.roots[0].place==(mode<2?2u:3u) && view.roots[0].value==1);
        if (mode>=2) CHECK(view.roots[1].place==6 && view.roots[1].value==(mode==2?2u:1u));
    } else CHECK(!view.cells.intrinsic_mask && !view.word_count && !view.root_count &&
        !view.dependencies && !view.roots);
    XrXirCellAccessRequest request={3,selected[mode],NULL,0};uint32_t access=99;
    status=xr_xir_compile_cell_access(context,proof,&request,&access);
    if (status!=XR_XIR_OK) return status;
    CHECK(access==(mode<4?XR_XIR_CELL_ACCESS_UNKNOWN:0));
    view=(XrXirCellClassOriginView){0};
    status=xr_xir_compile_cell_class_origin_view(context,proof,4,3,&view);
    if (status!=XR_XIR_OK) return status;
    CHECK(view.cells.intrinsic_mask==XR_XIR_CELL_ACCESS_UNKNOWN && !view.residual_intrinsic_mask &&
        view.word_count==1 && view.root_count==1 && view.dependencies[0]==1 &&
        view.roots[0].place==1 && view.roots[0].value==0);
    view=(XrXirCellClassOriginView){0};
    status=xr_xir_compile_cell_class_origin_view(context,proof,2,0,&view);
    if (status!=XR_XIR_OK) return status;
    CHECK(!view.cells.intrinsic_mask && view.cells.word_count==1 && view.cells.parameter_count==1 &&
        view.cells.parameters[0]==0 && view.cells.dependencies[0]==1 && !view.root_count && !view.dependencies);
    CHECK(xr_xir_cell_provenance_role(proof,2,0)==XR_XIR_CELL_PROOF_SCOPED_REF);
    return XR_XIR_OK;
}

static void cell_class_fact_reject(const XrXirCompileContext *context,XrXirCellProvenance *proof) {
    XrXirCellClassOriginView output={.residual_intrinsic_mask=7},saved=output;
    XrCompileResourceStats before=value_compile_stats(context);size_t sites=value_compile_calls;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,3,0,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&saved,sizeof(output)) && value_compile_calls==sites &&
        value_compile_stats(context).work==before.work);
    output=(XrXirCellClassOriginView){0};saved=output;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,5,0,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&saved,sizeof(output)));
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,3,UINT32_MAX,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&saved,sizeof(output)));
    size_t physical=value_compile_live,bytes=value_compile_bytes;
    XrXirCompileContext foreign=cell_proof_context(33554432);
    XrCompileResourceStats foreign_before=value_compile_stats(&foreign);
    uint64_t baseline=foreign_before.live_bytes;size_t foreign_sites=value_compile_calls;
    CHECK(xr_xir_compile_cell_class_origin_view(&foreign,proof,3,0,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&saved,sizeof(output)));
    XrCompileResourceStats foreign_after=value_compile_stats(&foreign);
    CHECK(foreign_after.allocation_count==foreign_before.allocation_count &&
        foreign_after.allocated_bytes==foreign_before.allocated_bytes &&
        foreign_after.live_bytes==foreign_before.live_bytes && foreign_after.peak_bytes==foreign_before.peak_bytes &&
        foreign_after.work==foreign_before.work && value_compile_calls==foreign_sites);
    fprintf(stdout,"CELL_CLASS_FOREIGN before%llu/%llu/%llu/%llu/%llu after%llu/%llu/%llu/%llu/%llu\n",
        (unsigned long long)foreign_before.allocation_count,(unsigned long long)foreign_before.allocated_bytes,
        (unsigned long long)foreign_before.live_bytes,(unsigned long long)foreign_before.peak_bytes,
        (unsigned long long)foreign_before.work,(unsigned long long)foreign_after.allocation_count,
        (unsigned long long)foreign_after.allocated_bytes,(unsigned long long)foreign_after.live_bytes,
        (unsigned long long)foreign_after.peak_bytes,(unsigned long long)foreign_after.work);
    xr_compile_resources_release(foreign.resources);
    CHECK(baseline && value_compile_live==physical && value_compile_bytes==bytes);
}

/* Mutable attacks target the owned observation only. Every original word
 * and subject is restored before the next fresh query or owner release. */
static void cell_class_fact_mutations(const XrXirCompileContext *context,XrXirCellProvenance *proof) {
    XrXirCellClassOriginView view={0},output={0};
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,4,3,&view)==XR_XIR_OK);
    CHECK(view.word_count==1 && view.root_count==1 && view.dependencies[0]==1);
    uint64_t *word=(uint64_t *)view.dependencies;uint64_t saved_word=*word;
    XrXirCellClassRoot *root=(XrXirCellClassRoot *)view.roots,saved_root=*root;
    *word=saved_word|(UINT64_C(1)<<63);
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,4,3,&output)==XR_XIR_BAD_STRUCTURE &&
        !output.cells.intrinsic_mask && !output.dependencies && !output.roots);
    *word=0;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,4,3,&output)==XR_XIR_BAD_STRUCTURE &&
        !output.cells.intrinsic_mask && !output.dependencies && !output.roots);
    *word=saved_word;root->value=1;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,4,3,&output)==XR_XIR_BAD_STRUCTURE &&
        !output.cells.intrinsic_mask && !output.dependencies && !output.roots);
    *root=saved_root;root->place=0;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,4,3,&output)==XR_XIR_BAD_STRUCTURE &&
        !output.cells.intrinsic_mask && !output.dependencies && !output.roots);
    *root=saved_root;
    CHECK(xr_xir_compile_cell_class_origin_view(context,proof,4,3,&output)==XR_XIR_OK &&
        output.cells.intrinsic_mask==XR_XIR_CELL_ACCESS_UNKNOWN && output.dependencies[0]==1 &&
        output.roots[0].place==1 && output.roots[0].value==0);
}

static XrXirStatus cell_class_fact_operation(const XrXirCompileContext *context,unsigned mode,bool oracle) {
    CellClassFactFixture fixture;cell_class_fact_fixture(&fixture,mode);
    XrXirDiagnostic diagnostic={0};XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xir_fixture_verify(context,&fixture.module,&diagnostic);
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,"CELL_CLASS_FACT mode%u phase0 status%u f%u b%u i%u\n",
        mode,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&proof,&diagnostic);
    memset(&fixture,0xCC,sizeof(fixture));
    if (status==XR_XIR_OK) status=cell_class_fact_view(context,proof,mode);
    if (status==XR_XIR_OK && oracle) {
        cell_class_fact_reject(context,proof);cell_class_fact_mutations(context,proof);
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}

static void cell_class_fact_cases(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    for (unsigned mode=0;mode<6;++mode) {
        XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
        CHECK(cell_class_fact_operation(&context,mode,true)==XR_XIR_OK);cell_proof_close(&context,baseline);
        context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
        size_t start=value_compile_calls;CHECK(cell_class_fact_operation(&context,mode,false)==XR_XIR_OK);
        size_t sites=value_compile_calls-start;XrCompileResourceStats exact=value_compile_stats(&context);
        CHECK(sites && exact.work);cell_proof_close(&context,baseline);
        fprintf(stdout,"CELL_CLASS_FACT_CENSUS mode%u sites%zu allocated%llu live%llu peak%llu work%llu\n",mode,sites,
            (unsigned long long)exact.allocated_bytes,(unsigned long long)exact.live_bytes,
            (unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
        for (size_t point=0;point<sites;++point) {
            context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
            size_t physical=value_compile_live,bytes=value_compile_bytes;
            value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
            CHECK(cell_class_fact_operation(&context,mode,false)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
                value_compile_live==physical && value_compile_bytes==bytes);
            value_compile_fail_at=SIZE_MAX;CHECK(cell_class_fact_operation(&context,mode,false)==XR_XIR_OK);
            cell_proof_close(&context,baseline);
        }
        for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=caps;
            if (!axis) limits.allocated_bytes=exact.allocated_bytes-minus;
            else if (axis==1) limits.live_bytes=exact.peak_bytes-minus;
            else limits.work=exact.work-minus;
            context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
            CHECK(cell_class_fact_operation(&context,mode,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            cell_proof_close(&context,baseline);
        }
    }
}
#endif // XIR_CELL_CLASS_FACT_CASES_H
