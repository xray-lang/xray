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
 *   The complete standard module and shared lexical module are inputs. Wrappers observe
 *   public results and the separately authenticated private cwd boundary.
 */
#ifndef XIR_PATH_COMPLETE_SOURCE_H
#define XIR_PATH_COMPLETE_SOURCE_H
static const char *const path_complete_names[]={"","completeResolve","completePair","completeNormalize","completeAlgorithms","completeCwd","completeFullpath"};
static const char path_complete_wrappers[]=
    "\n"
    "export fn completeResolve(raw:string)->string{return resolve(Path(raw)).raw}\n"
    "export fn completePair(a:string,b:string)->string{return resolve(Path(a),Path(b)).raw}\n"
    "export fn completeNormalize(raw:string)->string{return normalize(Path(raw)).raw}\n"
    "export fn completeAlgorithms()->i64{\n"
    " assert(lexical.root(\"/a\",false).absolute);assert(!lexical.root(\"relative\",false).absolute)\n"
    " assert(!lexical.root(\"C:/x\",false).absolute);assert(!lexical.root(\"C:\\\\x\",false).absolute)\n"
    " assert(lexical.normalize(\"a\\\\b\",false)==\"a\\\\b\")\n"
    " assert(lexical.joinAll([\"a\",\"b\"],false)==\"a/b\")\n"
    " assert(lexical.joinAll([\"a\",\"..\",\"b\"],false)==\"a/../b\")\n"
    " assert(lexical.dirname(\"/a/b.txt\",false)==\"/a\")\n"
    " assert(lexical.basename(\"/a/b.txt\",false)==\"b.txt\");assert(lexical.extname(\"/a/b.txt\",false)==\".txt\")\n"
    " assert(lexical.relative(\"/a/b\",\"/a/c\",false)==\"../c\")\n"
    " assert(lexical.root(\"C:/x\",true).absolute);assert(lexical.root(\"C:\\\\x\",true).absolute)\n"
    " assert(!lexical.root(\"C:x\",true).rooted);assert(!lexical.root(\"C:x\",true).absolute)\n"
    " assert(lexical.root(\"\\\\x\",true).rooted);assert(!lexical.root(\"\\\\x\",true).absolute)\n"
    " assert(lexical.root(\"\\\\\\\\srv\\\\share\",true).absolute)\n"
    " assert(!lexical.root(\"\\\\\\\\srv\",true).absolute);assert(!lexical.root(\"\\\\\\\\srv\\\\\",true).absolute)\n"
    " assert(!lexical.root(\"\\\\\\\\?\\\\UNC\",true).absolute);assert(!lexical.root(\"\\\\\\\\?\\\\uNc\\\\\",true).absolute)\n"
    " assert(!lexical.root(\"\\\\\\\\?\\\\C:\",true).absolute)\n"
    " assert(lexical.normalize(\"C:/a/../../b\",true)==\"C:\\\\b\")\n"
    " assert(lexical.joinAll([\"C:a\",\"b\"],true)==\"C:a\\\\b\")\n"
    " assert(lexical.joinAll([\"C:\\\\a\",\"C:b\"],true)==\"C:\\\\a\\\\b\")\n"
    " assert(lexical.joinAll([\"C:\\\\a\",\"D:b\"],true)==\"D:b\")\n"
    " assert(lexical.joinAll([\"C:\\\\a\",\"\\\\b\"],true)==\"C:\\\\b\")\n"
    " assert(lexical.joinAll([\"C:\\\\a\",\"..\",\"b\"],true)==\"C:\\\\a\\\\..\\\\b\")\n"
    " assert(lexical.normalize(\"\\\\\\\\?\\\\C:\\\\a\\\\..\\\\b/.\",true)==\"\\\\\\\\?\\\\C:\\\\a\\\\..\\\\b/.\")\n"
    " assert(lexical.normalize(\"\\\\\\\\.\\\\dev/a/../b\",true)==\"\\\\\\\\.\\\\dev/a/../b\")\n"
    " assert(lexical.basename(\"\\\\\\\\.\\\\dev/a\",true)==\"a\")\n"
    " assert(lexical.basename(\"\\\\\\\\?\\\\C:\\\\a/b\",true)==\"a/b\")\n"
    " assert(lexical.joinAll([\"\\\\\\\\?\\\\C:\\\\base\",\"D:\\\\x\"],true)==\"D:\\\\x\")\n"
    " assert(lexical.dirname(\"C:child\",true)==\"C:\");assert(lexical.basename(\"C:\",true)==\"\")\n"
    " assert(lexical.extname(\".\",true)==\"\");assert(lexical.extname(\"..\",true)==\"\")\n"
    " assert(lexical.relative(\"C:\\\\a\",\"D:\\\\b\",true)==null)\n"
    " assert(lexical.relative(\"C:\\\\A\",\"c:\\\\a\",true)==\"..\\\\a\")\n"
    " assert(lexical.relative(\"..\\\\a\",\"b\",true)==null)\n"
    " assert(lexical.relative(\"\\\\\\\\?\\\\C:\\\\a\",\"\\\\\\\\?\\\\C:\\\\b\",true)==null)\n"
    " if(_windows){assert(sep==\"\\\\\");assert(delimiter==\";\");assert(isAbsolute(Path(\"C:/x\")));assert(!isAbsolute(Path(\"/x\")))}\n"
    " else{assert(sep==\"/\");assert(delimiter==\":\");assert(isAbsolute(Path(\"/x\")));assert(!isAbsolute(Path(\"C:/x\")))}\n"
    " return 42\n"
    "}\n"
    "export fn completeCwd()->string{return __getcwd()}\n"
    "export fn completeFullpath(raw:string)->string{return __fullpath(raw)}\n"
    ;
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
    static const char *const queries[]={"__getcwd","__isWindows","__fullpath"};
    CHECK(module->function_count<=SIZE_MAX/sizeof(XrXirFunction));
    XrXirFunction *functions=xr_malloc((size_t)module->function_count*sizeof(*functions));CHECK(functions);
    memcpy(functions,module->functions,(size_t)module->function_count*sizeof(*functions));
    XrXirModule wrong=*module;wrong.functions=functions;
    for(unsigned n=0;n<3;++n){
        uint32_t target=path_complete_named(module,queries[n]);
        CHECK(xir_native_callable_verify(module,target)==XR_XIR_OK);
        functions[target].result=n==1?XR_XIR_STRING:XR_XIR_BOOL;
        CHECK(xir_native_callable_verify(&wrong,target)==XR_XIR_BAD_TYPE);
        functions[target]=module->functions[target];
    }
    uint32_t full=path_complete_named(module,"__fullpath");const XrXirType integer=XR_XIR_I64;
    functions[full].parameters=&integer;CHECK(xir_native_callable_verify(&wrong,full)==XR_XIR_BAD_TYPE);
    xr_free(functions);
    XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=path_complete_named(module,"completeCwd"),.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_NOT_SUPPORTED&&!result.type&&!result.reserved&&!result.payload);
    static const char *const frefs[]={
        "\nexport fn forbiddenCwd()->string{var f=__getcwd;return f()}\n",
        "\nexport fn forbiddenFlavour()->bool{var f=__isWindows;return f()}\n",
        "\nexport fn forbiddenFullpath()->string{var f=__fullpath;return f(\".\")}\n"
    };
    for(unsigned n=0;n<3;++n){
        size_t length=0;char *body=path_complete_text(&length);size_t suffix=strlen(frefs[n]);
        CHECK(length<=SIZE_MAX-suffix-1);char *negative=xr_malloc(length+suffix+1);CHECK(negative);
        memcpy(negative,body,length);memcpy(negative+length,frefs[n],suffix+1);xr_free(body);
        XrXirSourceResult rejected={0};
        CHECK(path_complete_check(context,negative,length+suffix,true,&rejected)==XR_XIR_BAD_TYPE&&!rejected.checked&&!rejected.snapshot);
        memset(negative,0xa5,length+suffix+1);xr_free(negative);xr_xir_compile_source_result_free(&rejected);
    }
    static const char *const fake[]={
        "export fn fake()->string{return __getcwd()}\n",
        "export fn fake()->bool{return __isWindows()}\n",
        "export fn fake()->string{return __fullpath(\".\")}\n"
    };
    for(unsigned n=0;n<3;++n){
        XrXirSourceResult rejected={0};
        CHECK(path_complete_check(context,fake[n],strlen(fake[n]),false,&rejected)==XR_XIR_BAD_VALUE&&!rejected.checked&&!rejected.snapshot);
        xr_xir_compile_source_result_free(&rejected);
    }
    XrXirModule changed=*module;XrXirDeclarations scope=*module->declarations;changed.declarations=&scope;
    size_t module_count=scope.module_count;CHECK(module_count&&module_count<=SIZE_MAX/sizeof(XrXirSourceModule));XrXirSourceModule *modules=xr_malloc(module_count*sizeof(*modules));CHECK(modules);
    memcpy(modules,scope.modules,(size_t)scope.module_count*sizeof(*modules));scope.modules=modules;uint32_t owner=scope.functions[native].module;CHECK(owner<scope.module_count);
    modules[owner].name="path";modules[owner].name_length=4;CHECK(xir_native_callable_verify(&changed,native)==XR_XIR_BAD_TYPE);xr_free(modules);
}
static XrXirArtifact *path_complete_raw_os_code(const XrXirCompileContext *context,uint32_t ids[PATH_COMPLETE_OS_COUNT]){
    static const char body[]="export fn rawCwd()->string{return __getcwd()}\n";
    char *text=xr_malloc(sizeof(body));CHECK(text);memcpy(text,body,sizeof(body));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"os",XR_PATH_ROOT "/stdlib"};
    XrXirSourceRequest request={session,XR_PATH_ROOT "/stdlib/os/os.xr",&authority,context,XR_PATH_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={"os/os.xr",text,sizeof(body)-1};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"raw OS cwd Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_compile_session_free(session);memset(text,0xa5,sizeof(body));xr_free(text);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t native=path_complete_named(module,"__getcwd");
    CHECK(module->declarations->functions[native].native_callable==64&&xir_native_callable_verify(module,native)==XR_XIR_OK);
    ids[PATH_COMPLETE_OS_ENTRY]=module->declarations->entry_function;ids[PATH_COMPLETE_OS_CWD]=path_complete_named(module,"rawCwd");return lowered;
}
static void path_complete_raw_os_emit(const XrXirArtifact *code,const uint32_t ids[PATH_COMPLETE_OS_COUNT],const char *path){
    XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,"path_complete_os_raw",16777216,&generated)==XR_XIR_OK);
    FILE *file=fopen(path,"ab");CHECK(file&&fwrite(generated.text,1,generated.length,file)==generated.length);
    CHECK(fprintf(file,"\nconst uint32_t path_complete_os_raw_ids[%u]={%u,%u};\n",PATH_COMPLETE_OS_COUNT,ids[0],ids[1])>0&&!fclose(file));
    xr_xir_compile_c_source_free(&generated);
}
static void path_complete_product(const XrXirCompileContext *context,const char *emit_base){
    size_t length=0;char *text=path_complete_text(&length);XrXirSourceResult source={0};CHECK(path_complete_check(context,text,length,true,&source)==XR_XIR_OK);
    memset(text,0xa5,length);xr_free(text);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[PATH_COMPLETE_COUNT]={module->declarations->entry_function};
    for(unsigned n=1;n<PATH_COMPLETE_COUNT;++n)ids[n]=path_complete_named(module,path_complete_names[n]);
    uint32_t raw_ids[PATH_COMPLETE_OS_COUNT]={0};XrXirArtifact *raw_code=path_complete_raw_os_code(context,raw_ids);
    if(emit_base){
        char path[2048];int count=snprintf(path,sizeof(path),"%s.complete.c",emit_base);CHECK(count>0&&(size_t)count<sizeof(path));
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(lowered,"path_complete",16777216,&generated)==XR_XIR_OK);FILE *out=fopen(path,"wb");CHECK(out);
        CHECK(fwrite(generated.text,1,generated.length,out)==generated.length&&fprintf(out,"\nconst uint32_t path_complete_ids[%u]={",PATH_COMPLETE_COUNT)>0);
        for(unsigned n=0;n<PATH_COMPLETE_COUNT;++n)CHECK(fprintf(out,"%s%u",n?",":"",ids[n])>0);
        CHECK(fprintf(out,"};\n")>0&&!fclose(out));xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);
        path_complete_raw_os_emit(raw_code,raw_ids,path);xr_xir_compile_artifact_free(raw_code);
    }else{
        path_complete_contracts(context,lowered);XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
        XrXirProgram *raw_program=NULL;CHECK(xr_xir_compile_vm_program_take(&raw_code,&raw_program)==XR_XIR_OK&&!raw_code);
        path_complete_execute(program,ids,raw_program,raw_ids);xr_xir_compile_program_drop(raw_program);xr_xir_compile_program_drop(program);
    }
}
#endif // XIR_PATH_COMPLETE_SOURCE_H
