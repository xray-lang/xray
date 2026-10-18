#define main materialization_original_main
#include "../p11_materialization/test_materialization.c"
#undef main
#include "xir/xxir_struct.h"
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_compile_domain.h"

typedef struct RecursiveInput { const char *root;char name[128],path[1024],text[8192];size_t length; } RecursiveInput;
typedef struct RecursiveWire { XrXirCompileContext producer;XrXirCheckedPacket packet; } RecursiveWire;
typedef struct RecursiveJob {
    XrXirCompileContext context;XrXirArtifact *lowered;XrXirCtfeCallRequest request;
    XrCompileResourceStats baseline;size_t blocks,bytes;
} RecursiveJob;
static void recursive_load(RecursiveInput *input,const char *name,bool plain,bool control) {
    *input=(RecursiveInput){0};CHECK(strlen(name)<sizeof(input->name));strcpy(input->name,name);
    if (!strcmp(name,"mode6")) return;
    input->root=control?CONTROL_ROOT:DEEP_ROOT;
    CHECK(snprintf(input->path,sizeof(input->path),"%s/%s",input->root,name)>0);
    FILE *file=fopen(input->path,"rb");CHECK(file);
    input->length=fread(input->text,1,sizeof(input->text)-1,file);
    CHECK(!ferror(file) && !fclose(file) && input->length<sizeof(input->text)-1);input->text[input->length]=0;
    if (plain) { char *marker=strstr(input->text,"comptime ");CHECK(marker && !strstr(marker+9,"comptime "));memset(marker,' ',9); }
}
static XrXirStatus recursive_source(const XrXirCompileContext *context,const RecursiveInput *input,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    if (!strcmp(input->name,"mode6")) return source_operation(context,6,XR_XIR_PROGRAM,output);
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->root};
    XrXirSourceRequest request={session,input->path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={input->name,input->text,input->length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void recursive_wire_new(RecursiveWire *wire,const RecursiveInput *input) {
    *wire=(RecursiveWire){0};context_new(&wire->producer);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=recursive_source(&wire->producer,input,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Recursive wire %s Source=%u %d:%d %s\n",input->name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);
    CHECK(xr_xir_compile_checked_write(source.checked,&wire->packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
}
static XrXirStatus recursive_pipeline(const XrXirCompileContext *context,const XrXirCheckedPacket *packet,XrXirArtifact **output) {
    XrXirArtifact *checked=NULL,*closed=NULL;XrXirStatus status=xr_xir_compile_checked_read(context,packet->bytes,packet->length,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(checked,&closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target,output,NULL);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);return status;
}
static void recursive_job_new(RecursiveJob *job,const RecursiveWire *wire,const char *function) {
    *job=(RecursiveJob){0};job->blocks=source_fixture_compile_live;job->bytes=source_fixture_compile_bytes;context_new(&job->context);
    CHECK(recursive_pipeline(&job->context,&wire->packet,&job->lowered)==XR_XIR_OK);
    job->request=(XrXirCtfeCallRequest){.evaluation={.artifact=job->lowered,
        .function=named(xr_xir_compile_artifact_module(job->lowered),function),.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(job->request.evaluation.checked_identity,job->lowered->checked_identity,32);job->baseline=stats(&job->context);
}
static void recursive_job_free(RecursiveJob *job) {
    CHECK(stats(&job->context).live_bytes==job->baseline.live_bytes);
    xr_xir_compile_artifact_free(job->lowered);xr_compile_resources_release(job->context.resources);
    CHECK(source_fixture_compile_live==job->blocks && source_fixture_compile_bytes==job->bytes);
}
static void recursive_value(const XrXirCompileContext *context,const XrXirValue *value,bool owning) {
    if (!owning) { CHECK(value->type==XR_XIR_I64 && value->payload==42);return; }
    const XrXirTypeArena *arena=xr_xir_value_arena(value);const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    XrXirCtfeTypeShape shape={0};CHECK(xr_xir_compile_ctfe_type_shape(context,types,(XrXirType)value->type,&shape)==XR_XIR_CTFE_OK);
    CHECK(shape.work<=UINT64_MAX/128 && xr_compile_resources_work(context->resources,shape.work*128)==XR_COMPILE_RESOURCE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_compile_value_domain_borrow(context->resources,value,arena,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=1000000,.scratch_bytes=1048576};
    XrXirValue scalar={0},children={0},child={0},leaf={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_struct_observe(value,0,&admission,&scalar)==XR_XIR_VALUE_OK && scalar.type==XR_XIR_I64 && scalar.payload==40);
    CHECK(xr_xir_struct_observe(value,1,&admission,&children)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_observe(&children,0,&admission,&child,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_observe(&child,0,&admission,&leaf)==XR_XIR_VALUE_OK && leaf.type==XR_XIR_I64 && leaf.payload==2);
    xr_xir_value_drop(&scalar);xr_xir_value_drop(&children);xr_xir_value_drop(&child);xr_xir_value_drop(&leaf);
}
static void recursive_execute(XrXirArtifact **lowered) {
    uint32_t entry=named(xr_xir_compile_artifact_module(*lowered),"answer");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(lowered,&program)==XR_XIR_OK && !*lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1048576;config.value_limit=1048576;config.call_limit=1048576;config.work_limit=1000000;
    XrXirInstance *instances[2]={NULL,NULL};
    for (unsigned i=0;i<2;++i) CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
}
