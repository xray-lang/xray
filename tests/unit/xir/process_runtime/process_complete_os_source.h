/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_complete_os_source.h - Original os.exec through full-module cold input
 *
 * KEY CONCEPT:
 *   The entire os source and its actual imports are checked without overlays.
 *   Capture and drain algorithms are neither extracted nor rewritten here.
 */
#ifndef PROCESS_COMPLETE_OS_SOURCE_H
#define PROCESS_COMPLETE_OS_SOURCE_H
static const char *const process_complete_os_names[]={"","completeExec","completeFields","cleanupOpen","cleanupClose","cleanupOrdinaryClose"};
static const char process_complete_os_wrappers[]=
    "\nexport fn completeExec(command:string,out:i64,error:i64)->ExecResult?{return exec(command,out,error)}\n"
    "export fn completeFields(result:ExecResult)->(Array<u8>,Array<u8>,i64,bool,bool){\n"
    " return(result.stdout,result.stderr,result.exitCode,result.stdoutTruncated,result.stderrTruncated)}\n"
    "export fn cleanupOpen()->(i64,i64)?{var opened=Pipe.open();if(opened==null){return null}\n"
    " return(opened!.readEnd(),opened!.writeEnd())}\n"
    "export fn cleanupClose(read:i64,write:i64)->bool{return __execPipeClose(read,write)}\n"
    "export fn cleanupOrdinaryClose(read:i64,write:i64)->bool{return Pipe(read,write).close()}\n"
    "export fn ctfeExecClose()->bool{return __execPipeClose(-1,-1)}\n";
static uint32_t process_complete_os_named(const XrXirModule *module,const char *name){
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return UINT32_MAX;
}
static char *process_complete_os_text(size_t *length){
    FILE *file=fopen(XR_PROCESS_ROOT "/stdlib/os/os.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));long bytes=ftell(file);CHECK(bytes>0&&!fseek(file,0,SEEK_SET));
    CHECK((uint64_t)bytes<=SIZE_MAX-sizeof(process_complete_os_wrappers));char *text=xr_malloc((size_t)bytes+sizeof(process_complete_os_wrappers));CHECK(text);
    CHECK(fread(text,1,(size_t)bytes,file)==(size_t)bytes&&!fclose(file));memcpy(text+(size_t)bytes,process_complete_os_wrappers,sizeof(process_complete_os_wrappers));
    *length=(size_t)bytes+sizeof(process_complete_os_wrappers)-1;return text;
}
static XrXirStatus process_complete_os_check(const XrXirCompileContext *context,const char *text,size_t length,bool genuine,XrXirSourceResult *output){
    XrModuleIdentityAuthority authority={genuine?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_MEMORY,"os",genuine?XR_PROCESS_ROOT "/stdlib":NULL};XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,genuine?XR_PROCESS_ROOT "/stdlib/os/os.xr":NULL,&authority,context,XR_PROCESS_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={genuine?"os/os.xr":NULL,text,length};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"complete os Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void process_complete_os_contracts(const XrXirCompileContext *context,const XrXirArtifact *code){
    const XrXirModule *module=xr_xir_compile_artifact_module(code);uint32_t native=process_complete_os_named(module,"__execPipeClose");
    CHECK(module->declarations->functions[native].native_callable==109&&xir_native_callable_verify(module,native)==XR_XIR_OK);
    XrXirModule changed=*module;XrXirDeclarations scope=*module->declarations;changed.declarations=&scope;
    size_t module_count=scope.module_count;CHECK(module_count&&module_count<=SIZE_MAX/sizeof(XrXirSourceModule));
    XrXirSourceModule *modules=xr_malloc(module_count*sizeof(*modules));CHECK(modules);
    memcpy(modules,scope.modules,module_count*sizeof(*modules));scope.modules=modules;uint32_t owner=scope.functions[native].module;CHECK(owner<scope.module_count);
    modules[owner].name="os";modules[owner].name_length=2;
    CHECK(xir_native_callable_verify(&changed,native)==XR_XIR_BAD_TYPE);xr_free(modules);
    XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=process_complete_os_named(module,"ctfeExecClose"),.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_NOT_SUPPORTED&&!value.type&&!value.reserved&&!value.payload);
    static const char *const denied[]={
        "\nexport fn badCloseRef()->bool{var f=__execPipeClose;return f(-1,-1)}\n",
        "\nfn badPipeWorker(p:Pipe)->i64{return p.readEnd()}\n"
        "export fn badPipeGo()->i64{var opened=Pipe.open();if(opened==null){return -1};var p=opened!\n"
        " var task=go badPipeWorker(p);return await task}\n"};
    size_t length=0;char *body=process_complete_os_text(&length);
    for(unsigned n=0;n<sizeof(denied)/sizeof(*denied);++n){
        size_t extra=strlen(denied[n]);CHECK(length<=SIZE_MAX-extra-1);char *negative=xr_malloc(length+extra+1);CHECK(negative);
        memcpy(negative,body,length);memcpy(negative+length,denied[n],extra+1);XrXirSourceResult rejected={0};
        CHECK(process_complete_os_check(context,negative,length+extra,true,&rejected)==XR_XIR_BAD_TYPE&&!rejected.checked&&!rejected.snapshot);
        memset(negative,0xa5,length+extra);xr_free(negative);xr_xir_compile_source_result_free(&rejected);
    }
    memset(body,0xa5,length);xr_free(body);
    static const char fake[]="export fn fakeExecClose(read:i64,write:i64)->bool{return __execPipeClose(read,write)}\n";
    XrXirSourceResult rejected={0};CHECK(process_complete_os_check(context,fake,sizeof(fake)-1,false,&rejected)==XR_XIR_BAD_VALUE&&!rejected.checked&&!rejected.snapshot);
    xr_xir_compile_source_result_free(&rejected);
}
static void process_complete_os_emit(const XrXirArtifact *code,const uint32_t *ids,const char *path,bool write){
    XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(code,"process_complete_os",16777216,&source)==XR_XIR_OK);
    char tail[256];int count=snprintf(tail,sizeof(tail),"\nconst uint32_t process_complete_os_ids[%u]={",PROCESS_COMPLETE_OS_COUNT);CHECK(count>0);
    for(unsigned n=0;n<PROCESS_COMPLETE_OS_COUNT;++n){int extra=snprintf(tail+count,sizeof(tail)-(size_t)count,"%s%u",n?",":"",ids[n]);CHECK(extra>0);count+=extra;}
    CHECK((size_t)count+4<sizeof(tail));memcpy(tail+count,"};\n",4);count+=3;FILE *file=fopen(path,write?"wb":"rb");CHECK(file);
    if(write)CHECK(fwrite(source.text,1,source.length,file)==source.length&&fwrite(tail,1,(size_t)count,file)==(size_t)count);
    else{for(size_t n=0;n<source.length;++n)CHECK(fgetc(file)==(unsigned char)source.text[n]);for(int n=0;n<count;++n)CHECK(fgetc(file)==(unsigned char)tail[n]);CHECK(fgetc(file)==EOF&&!ferror(file));}
    CHECK(!fclose(file));xr_xir_compile_c_source_free(&source);
}
static void process_complete_os_product(const XrXirCompileContext *context,const char *primary_c,bool emit){
    size_t length=0;char *text=process_complete_os_text(&length);XrXirSourceResult source={0};CHECK(process_complete_os_check(context,text,length,true,&source)==XR_XIR_OK);
    memset(text,0xa5,length);xr_free(text);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[PROCESS_COMPLETE_OS_COUNT]={module->declarations->entry_function};
    for(unsigned n=1;n<PROCESS_COMPLETE_OS_COUNT;++n)ids[n]=process_complete_os_named(module,process_complete_os_names[n]);
    char path[2048];int count=snprintf(path,sizeof(path),"%s.complete-os.c",primary_c);CHECK(count>0&&(size_t)count<sizeof(path));process_complete_os_emit(lowered,ids,path,emit);
    if(emit)xr_xir_compile_artifact_free(lowered);
    else{process_complete_os_contracts(context,lowered);XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
        process_complete_os_execute(program,ids);xr_xir_compile_program_drop(program);}
}
#endif // PROCESS_COMPLETE_OS_SOURCE_H
