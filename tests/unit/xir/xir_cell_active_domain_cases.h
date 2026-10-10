/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_active_domain_cases.h - Independent physical domains and owned exits
 */
#ifndef XIR_CELL_ACTIVE_DOMAIN_CASES_H
#define XIR_CELL_ACTIVE_DOMAIN_CASES_H
enum { CA_VALUES=512, CA_OPS=514 };
typedef struct CellActiveFixture {
    XrXirTypeNode node; XrXirTypes types; XrXirType parameter;
    XrXirInstruction init[3], operations[CA_OPS];
    XrXirBlock initial, block; XrXirFunction functions[2];
    XrXirFunctionIdentity identities[2]; XrXirSourceModule source;
    XrXirSlot slot; XrXirDeclarations declarations; XrXirModule module;
} CellActiveFixture;
static void cell_active_fixture(CellActiveFixture *f,unsigned mode) {
    CHECK(mode<3);memset(f,0,sizeof(*f));
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){.nodes=&f->node,.count=1};f->parameter=(XrXirType)256;
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};f->initial=(XrXirBlock){.count=1};
    for(uint32_t i=0;i<CA_VALUES;++i)
        f->operations[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    f->operations[CA_VALUES]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={CA_VALUES-1}};
    f->block=(XrXirBlock){.count=CA_VALUES+1};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->initial,.block_count=1,.instructions=f->init,.instruction_count=1};
    f->functions[1]=(XrXirFunction){.name="read",.name_length=4,.result=XR_XIR_I64,
        .blocks=&f->block,.block_count=1,.instructions=f->operations,.instruction_count=CA_VALUES+1};
    f->source=(XrXirSourceModule){.name="domain",.name_length=6,.initializer=0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=2,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    if(mode==1) {
        f->functions[1].parameters=&f->parameter;f->functions[1].parameter_count=1;
        f->operations[CA_VALUES]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
        f->operations[CA_VALUES+1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={CA_VALUES+1}};
        f->block.count=CA_OPS;f->functions[1].instruction_count=CA_OPS;
    } else if(mode==2) {
        /* No Cell type exists: a real Array root place still carries access. */
        f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
        f->slot=(XrXirSlot){.type=(XrXirType)256};
        f->declarations.slots=&f->slot;f->declarations.slot_count=1;
        f->init[0]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)256};
        f->init[1]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT};
        f->init[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
        f->initial.count=3;f->functions[0].instruction_count=3;
        f->operations[0]=(XrXirInstruction){.op=XR_XIR_SLOT_PLACE,.type=(XrXirType)256};
        f->operations[1]=(XrXirInstruction){.op=XR_XIR_ARRAY_LEN,.type=XR_XIR_I64};
        f->operations[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
        f->block.count=3;f->functions[1].instruction_count=3;
    }
}
static XrXirStatus cell_active_operation(const XrXirCompileContext *context,unsigned mode) {
    CellActiveFixture fixture;cell_active_fixture(&fixture,mode);
    XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&proof,NULL);
    if(status!=XR_XIR_OK) { CHECK(!proof);return status; }
    uint32_t count=fixture.functions[1].parameter_count+fixture.functions[1].instruction_count;
    memset(&fixture,0xCC,sizeof(fixture));
    for(uint32_t v=0;v<count && status==XR_XIR_OK;++v) {
        XrXirCellOriginView view={0};status=xr_xir_compile_cell_origin_view(context,proof,1,v,&view);
        if(status!=XR_XIR_OK) break;
        CHECK(view.intrinsic_mask==(mode==2 && !v?XR_XIR_CELL_ACCESS_ROOT:0));
        if(mode==1) {
            CHECK(view.parameter_count==1 && view.word_count==1 && view.parameters[0]==0);
            CHECK(view.dependencies[0]==(!v?UINT64_C(1):0));
        } else CHECK(!view.word_count && !view.parameter_count && !view.dependencies && !view.parameters);
    }
    if(status==XR_XIR_OK) {
        CHECK(xr_xir_cell_provenance_origin(proof,1,0)==(mode==1?XR_XIR_CELL_ORIGIN_SCOPED:
            mode==2?XR_XIR_CELL_ORIGIN_MODULE:0));
        CHECK(xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_UNKNOWN);
        XrXirCellOriginView sentinel={77,NULL,77,NULL,77},unchanged=sentinel;
        CHECK(xr_xir_compile_cell_origin_view(context,proof,1,count,&sentinel)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&sentinel,&unchanged,sizeof(sentinel)));
        CHECK(xr_xir_compile_cell_origin_view(context,proof,2,0,&sentinel)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&sentinel,&unchanged,sizeof(sentinel)));
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
/* Independent physical IDs deliberately collide with the following active
 * span: f1 and f2 both begin at 4; f3 and f4 both begin at 7. */
typedef struct CellInterleavedFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType cell, scalar;
    XrXirInstruction initial[4], constants[3], read[2], plain;
    XrXirBlock four, three, pair, one; uint32_t capture;
    XrXirFunction functions[5]; XrXirFunctionIdentity identities[5];
    XrXirSourceModule source; XrXirDeclarations declarations; XrXirModule module;
} CellInterleavedFixture;
static void cell_interleaved_fixture(CellInterleavedFixture *f) {
    memset(f,0,sizeof(*f));f->cell=(XrXirType)256;f->scalar=XR_XIR_I64;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};f->capture=1;
    f->initial[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41};
    f->initial[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)256};
    f->initial[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,
        .args={0,1},.immediate=2};
    f->initial[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->constants[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=8};
    f->constants[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
    f->constants[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->read[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->read[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->plain=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->four=(XrXirBlock){.count=4};f->three=(XrXirBlock){.count=3};
    f->pair=(XrXirBlock){.count=2};f->one=(XrXirBlock){.count=1};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->four,.block_count=1,.instructions=f->initial,.instruction_count=4,
        .operands=&f->capture,.operand_count=1};
    f->functions[1]=(XrXirFunction){.name="constants",.name_length=9,.result=XR_XIR_I64,
        .blocks=&f->three,.block_count=1,.instructions=f->constants,.instruction_count=3};
    f->functions[2]=(XrXirFunction){.name="capture",.name_length=7,.result=XR_XIR_I64,
        .parameters=&f->cell,.parameter_count=1,.blocks=&f->pair,.block_count=1,
        .instructions=f->read,.instruction_count=2};
    f->functions[3]=(XrXirFunction){.name="plain",.name_length=5,.result=XR_XIR_I64,
        .parameters=&f->scalar,.parameter_count=1,.blocks=&f->one,.block_count=1,
        .instructions=&f->plain,.instruction_count=1};
    f->functions[4]=(XrXirFunction){.name="open",.name_length=4,.result=XR_XIR_I64,
        .parameters=&f->cell,.parameter_count=1,.blocks=&f->pair,.block_count=1,
        .instructions=f->read,.instruction_count=2};
    f->source=(XrXirSourceModule){.name="domains",.name_length=7,.initializer=0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
static XrXirStatus cell_interleaved_operation(const XrXirCompileContext *context) {
    CellInterleavedFixture fixture;cell_interleaved_fixture(&fixture);
    XrXirCellProvenance *proof=NULL;
    XrXirStatus status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&proof,NULL);
    if(status!=XR_XIR_OK) { CHECK(!proof);return status; }
    XrXirCellProvenance *occupied=proof;
    CHECK(xr_xir_compile_cell_provenance_verified(context,&fixture.module,&occupied,NULL)==
        XR_XIR_BAD_STRUCTURE && occupied==proof);
    memset(&fixture,0xCC,sizeof(fixture));
    const uint32_t counts[]={4,3,3,2,3};
    for(uint32_t f=0;f<5 && status==XR_XIR_OK;++f) {
        for(uint32_t v=0;v<counts[f] && status==XR_XIR_OK;++v) {
            XrXirCellOriginView view={0};status=xr_xir_compile_cell_origin_view(context,proof,f,v,&view);
            if(status!=XR_XIR_OK) break;
            CHECK(!view.intrinsic_mask);
            if(f==4) CHECK(view.parameter_count==1 && view.parameters[0]==0 && view.word_count==1 &&
                view.dependencies[0]==(!v?UINT64_C(1):0));
            else CHECK(!view.word_count && !view.parameter_count && !view.parameters && !view.dependencies);
            uint32_t origin=((f==0 && v==1) || (f==2 && !v))?XR_XIR_CELL_ORIGIN_OWNED:
                f==4 && !v?XR_XIR_CELL_ORIGIN_SCOPED:0;
            CHECK(xr_xir_cell_provenance_origin(proof,f,v)==origin);
        }
        if(status==XR_XIR_OK) {
            XrXirCellOriginView output={77,NULL,77,NULL,77},unchanged=output;
            CHECK(xr_xir_compile_cell_origin_view(context,proof,f,counts[f],&output)==XR_XIR_BAD_STRUCTURE &&
                !memcmp(&output,&unchanged,sizeof(output)));
        }
    }
    if(status==XR_XIR_OK) {
        CHECK(xr_xir_cell_provenance_role(proof,2,0)==XR_XIR_CELL_PROOF_OWNED_CAPTURE);
        CHECK(xr_xir_cell_provenance_role(proof,3,0)==XR_XIR_CELL_PROOF_UNKNOWN);
        CHECK(xr_xir_cell_provenance_role(proof,4,0)==XR_XIR_CELL_PROOF_UNKNOWN);
        const XrXirCellAccessRequest requests[]={{3,0,NULL,0},{4,0,NULL,0}};
        for(unsigned r=0;r<2 && status==XR_XIR_OK;++r) {
            uint32_t access=99;status=xr_xir_compile_cell_access(context,proof,&requests[r],&access);
            if(status==XR_XIR_OK) CHECK(access==(r?XR_XIR_CELL_ACCESS_UNKNOWN:0));
        }
    }
    xr_xir_compile_cell_provenance_free(proof);return status;
}
static XrXirStatus cell_active_selected_operation(const XrXirCompileContext *context,unsigned mode) {
    return mode==3?cell_interleaved_operation(context):cell_active_operation(context,mode);
}
static XrXirCompileContext cell_active_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);return context;
}
static void cell_active_domains(void) {
    const XrCompileResourceLimits caps={33554432,33554432,33554432};
    for(unsigned mode=0;mode<4;++mode) {
        XrXirCompileContext context=cell_active_context(caps);
        uint64_t baseline=value_compile_stats(&context).live_bytes;
        XrXirDiagnostic diagnostic={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
        XrXirStatus verified;
        if(mode==3) {
            CellInterleavedFixture fixture;cell_interleaved_fixture(&fixture);
            verified=xir_fixture_verify(&context,&fixture.module,&diagnostic);
        } else {
            CellActiveFixture fixture;cell_active_fixture(&fixture,mode);
            verified=xir_fixture_verify(&context,&fixture.module,&diagnostic);
        }
        if(verified!=XR_XIR_OK)
            fprintf(stderr,"CELL_ACTIVE_VERIFY stagefull mode%u status%u diagnostic%u f%u b%u i%u reason%u\n",
                mode,(unsigned)verified,(unsigned)diagnostic.status,diagnostic.function,diagnostic.block,
                diagnostic.instruction,(unsigned)diagnostic.reason);
        CHECK(verified==XR_XIR_OK);
        cell_proof_close(&context,baseline);
        context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
        size_t start=value_compile_calls;
        CHECK(cell_active_selected_operation(&context,mode)==XR_XIR_OK);
        size_t sites=value_compile_calls-start;XrCompileResourceStats exact=value_compile_stats(&context);
        CHECK(sites && exact.work);
        fprintf(stdout,"CELL_ACTIVE_CENSUS mode%u sites%zu allocated%llu live%llu peak%llu work%llu\n",
            mode,sites,(unsigned long long)exact.allocated_bytes,(unsigned long long)exact.live_bytes,
            (unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
        cell_proof_close(&context,baseline);
        for(size_t point=0;point<sites;++point) {
            context=cell_active_context(caps);baseline=value_compile_stats(&context).live_bytes;
            size_t physical=value_compile_live,bytes=value_compile_bytes;
            value_compile_fail_at=value_compile_calls+point;value_compile_injected=false;
            CHECK(cell_active_selected_operation(&context,mode)==XR_XIR_OUT_OF_MEMORY && value_compile_injected &&
                value_compile_live==physical && value_compile_bytes==bytes);
            value_compile_fail_at=SIZE_MAX;
            CHECK(cell_active_selected_operation(&context,mode)==XR_XIR_OK);cell_proof_close(&context,baseline);
        }
        for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=caps;
            if(axis==0) limits.allocated_bytes=exact.allocated_bytes-minus;
            else if(axis==1) limits.live_bytes=exact.peak_bytes-minus;
            else limits.work=exact.work-minus;
            context=cell_active_context(limits);baseline=value_compile_stats(&context).live_bytes;
            XrXirStatus status=cell_active_selected_operation(&context,mode);
            CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));cell_proof_close(&context,baseline);
        }
    }
    /* This independent malformed physical input cannot publish an empty proof. */
    XrXirCompileContext context=cell_active_context(caps);uint64_t baseline=value_compile_stats(&context).live_bytes;
    CellActiveFixture fixture;cell_active_fixture(&fixture,0);fixture.operations[0].op=XR_XIR_OP_COUNT;
    XrXirCellProvenance *proof=NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL)==XR_XIR_BAD_STRUCTURE && !proof);
    cell_active_fixture(&fixture,0);fixture.functions[1].instructions=NULL;
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&proof,NULL)==XR_XIR_BAD_STRUCTURE && !proof);
    cell_proof_close(&context,baseline);
    puts("Cell active domains: unused type, open scoped formal, Cell-free Array place, producer death, full FI and three axes");
}
#endif // XIR_CELL_ACTIVE_DOMAIN_CASES_H
