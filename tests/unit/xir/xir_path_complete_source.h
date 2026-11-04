/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_complete_source.h - Entire Path source through cold VM and same C
 *
 * KEY CONCEPT:
 *   All standard algorithms remain byte-for-byte inputs. Wrappers observe
 *   public results and the separately authenticated private cwd boundary.
 */
#ifndef XIR_PATH_COMPLETE_SOURCE_H
#define XIR_PATH_COMPLETE_SOURCE_H
static const char *const path_complete_names[]={"","completeResolve","completePair","completeNormalize","completeAlgorithms","completeCwd"};
static const char path_complete_wrappers[]=
    "\nexport fn completeResolve(raw:string)->string{return resolve(Path(raw)).raw}\n"
    "export fn completePair(a:string,b:string)->string{return resolve(Path(a),Path(b)).raw}\n"
    "export fn completeNormalize(raw:string)->string{return normalize(Path(raw)).raw}\n"
    "export fn completeAlgorithms()->i64{\n"
    " assert(isAbsolute(Path(\"/a\")));assert(!isAbsolute(Path(\"relative\")))\n"
    " assert(join(Path(\"a\"),Path(\"b\")).raw==\"a/b\")\n"
    " assert(dirname(Path(\"/a/b.txt\")).raw==\"/a\")\n"
    " assert(basename(Path(\"/a/b.txt\"))==\"b.txt\");assert(extname(Path(\"/a/b.txt\"))==\".txt\")\n"
    " var info=parse(Path(\"/a/b.txt\"));assert(format(info).raw==\"/a/b.txt\")\n"
    " assert(relative(Path(\"/a/b\"),Path(\"/a/c\")).raw==\"../c\");return 42}\n"
    "export fn completeCwd()->string{return __getcwd()}\n";
static uint32_t path_complete_named(const XrXirModule *module,const char *name){
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return UINT32_MAX;
}
static char *path_complete_text(size_t *length){
    FILE *file=fopen(XR_PATH_ROOT "/stdlib/path/path.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));long bytes=ftell(file);CHECK(bytes>0&&!fseek(file,0,SEEK_SET));
    CHECK((uint64_t)bytes<=SIZE_MAX-sizeof(path_complete_wrappers));char *text=xr_malloc((size_t)bytes+sizeof(path_complete_wrappers));CHECK(text);
    CHECK(fread(text,1,(size_t)bytes,file)==(size_t)bytes&&!fclose(file));memcpy(text+(size_t)bytes,path_complete_wrappers,sizeof(path_complete_wrappers));
    *length=(size_t)bytes+sizeof(path_complete_wrappers)-1;return text;
}
static XrXirStatus path_complete_check(const XrXirCompileContext *context,const char *text,size_t length,
    bool genuine,XrXirSourceResult *output){
    XrModuleIdentityAuthority authority={genuine?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_MEMORY,"path",genuine?XR_PATH_ROOT "/stdlib":NULL};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,genuine?XR_PATH_ROOT "/stdlib/path/path.xr":NULL,&authority,context,XR_PATH_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};XrXirSourceText input={genuine?"path/path.xr":NULL,text,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"complete Path Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void path_complete_contracts(const XrXirCompileContext *context,const XrXirArtifact *code){
    const XrXirModule *module=xr_xir_compile_artifact_module(code);uint32_t native=path_complete_named(module,"__getcwd");
    CHECK(module->declarations->functions[native].native_callable==108&&xir_native_callable_verify(module,native)==XR_XIR_OK);
    XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=path_complete_named(module,"completeCwd"),.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_NOT_SUPPORTED&&!result.type&&!result.reserved&&!result.payload);
    static const char fref[]="\nexport fn forbiddenCwd()->string{var f=__getcwd;return f()}\n";
    size_t length=0;char *body=path_complete_text(&length);CHECK(length<=SIZE_MAX-sizeof(fref));char *negative=xr_malloc(length+sizeof(fref));CHECK(negative);
    memcpy(negative,body,length);memcpy(negative+length,fref,sizeof(fref));xr_free(body);XrXirSourceResult rejected={0};
    CHECK(path_complete_check(context,negative,length+sizeof(fref)-1,true,&rejected)==XR_XIR_BAD_TYPE&&!rejected.checked&&!rejected.snapshot);
    memset(negative,0xa5,length+sizeof(fref));xr_free(negative);xr_xir_compile_source_result_free(&rejected);
    static const char fake[]="export fn fake()->string{return __getcwd()}\n";
    CHECK(path_complete_check(context,fake,sizeof(fake)-1,false,&rejected)==XR_XIR_BAD_VALUE&&!rejected.checked&&!rejected.snapshot);xr_xir_compile_source_result_free(&rejected);
    XrXirModule changed=*module;XrXirDeclarations scope=*module->declarations;changed.declarations=&scope;
    size_t module_count=scope.module_count;CHECK(module_count&&module_count<=SIZE_MAX/sizeof(XrXirSourceModule));XrXirSourceModule *modules=xr_malloc(module_count*sizeof(*modules));CHECK(modules);
    memcpy(modules,scope.modules,(size_t)scope.module_count*sizeof(*modules));scope.modules=modules;uint32_t owner=scope.functions[native].module;CHECK(owner<scope.module_count);
    modules[owner].name="path";modules[owner].name_length=4;CHECK(xir_native_callable_verify(&changed,native)==XR_XIR_BAD_TYPE);xr_free(modules);
}
static void path_complete_product(const XrXirCompileContext *context,const char *emit_base){
    size_t length=0;char *text=path_complete_text(&length);XrXirSourceResult source={0};CHECK(path_complete_check(context,text,length,true,&source)==XR_XIR_OK);
    memset(text,0xa5,length);xr_free(text);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[PATH_COMPLETE_COUNT]={module->declarations->entry_function};
    for(unsigned n=1;n<PATH_COMPLETE_COUNT;++n)ids[n]=path_complete_named(module,path_complete_names[n]);
    if(emit_base){
        char path[2048];int count=snprintf(path,sizeof(path),"%s.complete.c",emit_base);CHECK(count>0&&(size_t)count<sizeof(path));
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(lowered,"path_complete",16777216,&generated)==XR_XIR_OK);FILE *out=fopen(path,"wb");CHECK(out);
        CHECK(fwrite(generated.text,1,generated.length,out)==generated.length&&fprintf(out,"\nconst uint32_t path_complete_ids[%u]={",PATH_COMPLETE_COUNT)>0);
        for(unsigned n=0;n<PATH_COMPLETE_COUNT;++n)CHECK(fprintf(out,"%s%u",n?",":"",ids[n])>0);
        CHECK(fprintf(out,"};\n")>0&&!fclose(out));xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);
    }else{
        path_complete_contracts(context,lowered);XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
        path_complete_execute(program,ids);xr_xir_compile_program_drop(program);
    }
}
#endif // XIR_PATH_COMPLETE_SOURCE_H
