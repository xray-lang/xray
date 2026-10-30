#define main materialization_original_main
#include "../p11_materialization/test_materialization.c"
#undef main

int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);bool plain=argc==3 && !strcmp(argv[2],"--plain");
    bool aggregate=argc==3 && !strcmp(argv[2],"--aggregate");
    CHECK(argc==2 || plain || aggregate);char path[1024],text[8192];
    CHECK(snprintf(path,sizeof(path),DEEP_ROOT "/%s",argv[1])>0);FILE *file=fopen(path,"rb");CHECK(file);
    size_t length=fread(text,1,sizeof(text)-1,file);CHECK(!ferror(file) && !fclose(file) && length<sizeof(text)-1);text[length]=0;
    if (plain) { char *marker=strstr(text,"comptime ");CHECK(marker && !strstr(marker+9,"comptime "));memset(marker,' ',9); }
    XrXirCompileContext context;context_new(&context);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,DEEP_ROOT};
    XrXirSourceRequest request={session,path,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={argv[1],text,length};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    fprintf(stderr,"Deep loan %s plain=%u Source=%u %u:%d:%d %s\n",argv[1],plain,status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    xr_compile_session_free(session);memset(text,0xa5,sizeof(text));
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);const XrXirFunction *answer=&module->functions[named(module,"answer")];
    if (aggregate) {
        CHECK(xr_xir_compile_construction_materialization_count(source.checked->construction)==1);
        bool materialized=false;
        for (uint32_t i=0;i<answer->instruction_count;++i) {
            const XrXirInstruction *op=&answer->instructions[i];
            if (op->op==XR_XIR_CALL && (uint64_t)op->immediate<module->function_count)
                materialized|=module->declarations->functions[op->immediate].method_kind==XR_XIR_MATERIALIZER;
        }
        CHECK(materialized);
    } else if (!plain) {
        bool literal=false;
        for (uint32_t i=0;i<answer->instruction_count;++i) {
            const XrXirInstruction *op=&answer->instructions[i];
            CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_INVOKE_INDIRECT);
            literal|=op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64 && op->immediate==42;
        }
        CHECK(literal);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t entry=named(xr_xir_compile_artifact_module(lowered),"answer");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);xr_compile_resources_release(context.resources);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1048576;config.value_limit=1048576;config.call_limit=1048576;config.work_limit=1000000;
    XrXirInstance *instances[2]={NULL,NULL};
    for (unsigned i=0;i<2;++i) CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Deep variable data loan %s plain=%u: COW snapshots, two real Instance42, parser/caller/Program dead, physical0\n",argv[1],plain);
    return 0;
}
