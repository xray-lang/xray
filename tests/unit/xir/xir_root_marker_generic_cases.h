/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_marker_generic_cases.h - Full generic marker reads without callbacks
 */
#ifndef XIR_ROOT_MARKER_GENERIC_CASES_H
#define XIR_ROOT_MARKER_GENERIC_CASES_H
typedef struct MarkerGenericFixture {
    XrXirTypeNode nodes[5];
    XrXirType arguments[1],fields[1];
    XrXirConstraint bound;
    XrXirInterfaceApplication requirement;
    XrXirInterfaceDeclaration interface;
    XrXirInterfaceTable interfaces;
    XrXirNominalDeclaration nominals[2];
    XrXirNominalField field;
    XrXirNominalVariant variant;
    XrXirNominalTable table;
    XrXirTypes types;
    XrXirImplementation implementation;
    XrXirImplementationTable implementations;
    XrXirInstruction instruction;
    XrXirBlock block;
    XrXirFunction function;
    XrXirFunctionIdentity identity;
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} MarkerGenericFixture;
static void mr_generic_fixture(MarkerGenericFixture *f,bool projected) {
    memset(f,0,sizeof(*f));
    f->nominals[0]=(XrXirNominalDeclaration){.module={"marker",6},.name={"Token",5},
        .kind=XR_XIR_NOMINAL_STRUCT,.exported=1};
    f->variant=(XrXirNominalVariant){.name={"Only",4},.field_count=projected?1u:0u};
    f->nominals[1]=(XrXirNominalDeclaration){.module={"marker",6},.name={"Box",3},.exported=1,
        .kind=XR_XIR_NOMINAL_ENUM,.parameter_count=1,.constraints=&f->bound,
        .variants=&f->variant,.variant_count=1};
    f->table=(XrXirNominalTable){.declarations=f->nominals,.count=2};
    if(projected) {
        f->bound.markers=XR_XIR_CONSTRAINT_SENDABLE;f->arguments[0]=XR_XIR_I64;
        f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
        f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1};
        f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256};
        f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)257,.parameter_span=1};
        f->field=(XrXirNominalField){.name={"value",5},.type=(XrXirType)259};
        f->nominals[1].fields=&f->field;f->nominals[1].field_count=1;f->fields[0]=(XrXirType)258;
    } else {
        f->interface=(XrXirInterfaceDeclaration){.module={"marker",6},.name={"Tag",3},.exported=1};
        f->interfaces=(XrXirInterfaceTable){&f->interface,1};
        f->requirement=(XrXirInterfaceApplication){0};
        f->bound=(XrXirConstraint){.interfaces=&f->requirement,.interface_count=1};
        f->implementation=(XrXirImplementation){.nominal_declaration=0};
        f->implementations=(XrXirImplementationTable){&f->implementation,1};
        f->arguments[0]=(XrXirType)256;f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL};
    }
    uint32_t last=projected?4u:1u;
    f->nodes[last]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1,
        .arguments=f->arguments,.argument_count=1,.fields=projected?f->fields:NULL,.field_count=projected?1u:0u}};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=last+1,.nominals=&f->table,
        .interfaces=projected?NULL:&f->interfaces};
    f->instruction=(XrXirInstruction){.op=XR_XIR_RETURN};f->block=(XrXirBlock){.count=1};
    f->function=(XrXirFunction){.name="init",.name_length=4,.instructions=&f->instruction,
        .instruction_count=1,.blocks=&f->block,.block_count=1};
    f->source=(XrXirSourceModule){.name="marker",.name_length=6};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=&f->identity,
        .implementations=projected?NULL:&f->implementations,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=&f->function,.function_count=1,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static void mr_forbid_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static XrXirStatus mr_marker_attempt(const XrXirCompileContext *work,const XrXirModule *module,
    XrXirType type,XirReadScratch *memory) {
    XrXirStatus status=xir_read_scratch_begin(work,memory);if(status!=XR_XIR_OK) return status;
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    size_t allocated=attempts;uint64_t physical=live_bytes;
    CHECK(!pc_allocate_hook && !pc_free_hook);pc_allocate_hook=mr_forbid_callback;pc_free_hook=mr_forbid_callback;
    status=xir_metadata_type_markers_read(work,&proof,type,XR_XIR_CONSTRAINT_SENDABLE,memory);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && live_bytes==physical);return status;
}
static XrXirStatus mr_marker_loop(const XrXirCompileContext *work,const XrXirModule *module,
    XrXirType type,XirReadScratch *memory) {
    for(unsigned retry=0;retry<64;++retry) {
        XrXirStatus status=mr_marker_attempt(work,module,type,memory);
        if(status!=XR_XIR_BUDGET || !memory->required) return status;
        status=xir_read_scratch_grow(work,memory);if(status!=XR_XIR_OK) return status;
    }
    CHECK(false);return XR_XIR_BAD_STRUCTURE;
}
static void mr_generic_cases(bool projected) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    MarkerGenericFixture f;mr_generic_fixture(&f,projected);XrXirArtifact *checked=NULL;
    XrXirStatus status=xir_fixture_check(&context,&f.module,&checked,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"generic fixture projected%u status%u\n",projected,status);
    CHECK(status==XR_XIR_OK && checked);memset(&f,0xD3,sizeof(f));
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+module->types->count-1);
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    CHECK(xir_metadata_type_markers_prove(&context,&proof,type,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    XirReadScratch memory={.resources=context.resources};size_t start=attempts;
    CHECK(mr_marker_loop(&context,module,type,&memory)==XR_XIR_OK);size_t sites=attempts-start;CHECK(sites && sites<64);
    CHECK(mr_marker_attempt(&context,module,type,&memory)==XR_XIR_OK);
    if(projected) {
        XrXirType *field=(XrXirType *)module->types->nodes[4].nominal.fields;XrXirType saved=*field;*field=XR_XIR_BOOL;
        CHECK(mr_marker_attempt(&context,module,type,&memory)==XR_XIR_BAD_TYPE);*field=saved;
    } else {
        XrXirImplementation *impl=(XrXirImplementation *)module->declarations->implementations->records;
        impl->interface.declaration=1;CHECK(mr_marker_attempt(&context,module,type,&memory)==XR_XIR_BAD_STRUCTURE);
        impl->interface.declaration=0;
    }
    CHECK(mr_marker_attempt(&context,module,type,&memory)==XR_XIR_OK);xir_read_scratch_free(&memory);
    uint64_t held=pc_stats(&context).live_bytes;
    for(size_t site=0;site<sites;++site) {
        memory=(XirReadScratch){.resources=context.resources};fail_at=attempts+site;injected=false;
        status=mr_marker_loop(&context,module,type,&memory);fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);xir_read_scratch_free(&memory);
        CHECK(pc_stats(&context).live_bytes==held);memory=(XirReadScratch){.resources=context.resources};
        CHECK(mr_marker_loop(&context,module,type,&memory)==XR_XIR_OK);xir_read_scratch_free(&memory);
        CHECK(pc_stats(&context).live_bytes==held);
    }
    xr_xir_compile_artifact_free(checked);pc_release(&context,baseline);
    printf("generic marker projected%u: full admission, zero-callback attempts, %zu allocation failures, reused-capacity mutation rejection, physical0\n",projected,sites);
}
#endif // XIR_ROOT_MARKER_GENERIC_CASES_H
