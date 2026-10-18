/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_copy_flow.c - Original Source flow and boundary assertions
 *
 * KEY CONCEPT:
 *   Reuse the unchanged independent flow/query/codec assertions. The complete
 *   original test remains registered with its separate error rejection cases.
 */
#define main callable_root_source_original_checks
#include "test_xir_callable_root_source.c"
#undef main

static const char copy_flow_text[]=
    "var state:i64=23\n"
    "fn plain()->i64{return 7}\n"
    "fn leaf()->i64{return state}\n"
    "fn joined(flag:bool)->i64{const f=flag ? plain : leaf;return f()}\n"
    "fn fixed()->i64{const f:fn()->i64=plain;return f()}\n"
    "fn main()->i64{return joined(true)}\n";

static void copy_flow_facts(XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
    const char *names[2]={"joined","fixed"};
    for (uint32_t f=0;f<2;++f) {
        uint32_t index=root_source_function(module,names[f]);
        const XrXirRootEffects *facts=xr_xir_effects_root(effects,index);
        CHECK(facts && facts->requires_root==!f && facts->unresolved==!!f);
        const XrXirFunction *body=&module->functions[index];bool normalized=false;
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (body->instructions[i].op!=XR_XIR_FUNCTION_WEAKEN) continue;
            const XrXirTypeNode *type=xr_xir_callable_signature(module->types,body->instructions[i].type);
            if (type && type->flags==(f?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED))
                normalized=true;
        }
        CHECK(normalized);
    }
    xr_xir_compile_effects_free(effects);
}

static XrXirStatus copy_flow_operation(const XrXirCompileContext *context,void *opaque) {
    bool inspect=*(const bool *)opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK)
        return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-copy-flow",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,copy_flow_text,sizeof(copy_flow_text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if (status==XR_XIR_OK && inspect) copy_flow_facts(result.checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(result.checked,&packet,NULL);
    xr_xir_compile_source_result_free(&result);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
    if (status==XR_XIR_OK && inspect) copy_flow_facts(decoded);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_free(failure);return status;
}

int main(int argc,char **argv) {
    bool inspect=argc==1;
    if (argc==2 && !strcmp(argv[1],"--compiler"))
        library_compile_operation_cases("Source copy join and Checked replay",copy_flow_operation,&inspect);
    else if (argc==1 || (argc==2 && !strcmp(argv[1],"--census"))) {
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;CHECK(copy_flow_operation(&owner.context,&inspect)==XR_XIR_OK);
        printf("Source copy join census: %zu actual allocation sites\n",source_program_compile_attempts);
        if (inspect) {
            RootSourceOperation fixture={"flows.xr",true};
            CHECK(root_source_operation(&owner.context,&fixture)==XR_XIR_OK);
        }
        library_compile_owner_drop(&owner);
    } else return 2;
    library_compile_observer_free();
    if (inspect) puts("Source copy flow: original 23 ROOT/UNKNOWN assertions, query, producer death and Checked replay");
    return 0;
}
