/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_flavours_source.h - Public lexical types through cold checked input
 */
#ifndef XIR_PATH_FLAVOURS_SOURCE_H
#define XIR_PATH_FLAVOURS_SOURCE_H
#include "base/xsha256.h"
#include "xir/xxir_stdlib_internal.h"
static void path_previous134(const XrXirCompileContext *context){
    _Static_assert(XR_XIR_CHECKED_CONTRACT>134,"the native Path reader excludes the projected-cwd contract");
    FILE *file=fopen(XR_PATH_FIXTURES "/previous134-native-path/old134-path.xrchk","rb");CHECK(file);
    unsigned char *packet=xr_malloc(69634);CHECK(packet);size_t length=fread(packet,1,69634,file);
    CHECK(length==69633&&feof(file)&&!ferror(file)&&!fclose(file));
    static const uint8_t expected[32]={0x41,0x64,0xf1,0x6d,0xb5,0x9c,0x37,0x36,0xa8,0xae,0x84,0x38,0x16,0x81,0xa6,0xfd,0x74,0xb7,0xd2,0x12,0x15,0x43,0x14,0x81,0x38,0xa1,0xad,0xba,0x04,0xf3,0x8e,0x93};uint8_t digest[32];
    xr_sha256(packet,length,digest);CHECK(!memcmp(digest,expected,sizeof(digest)));
    CHECK(packet[8]==42&&packet[12]==134);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    uint32_t future=XR_XIR_CHECKED_CONTRACT+1;for(unsigned i=0;i<4;++i)packet[12+i]=(uint8_t)(future>>(i*8));
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    memset(packet,0xa5,length);xr_free(packet);
}
static void path_previous135(const XrXirCompileContext *context){
    _Static_assert(XR_XIR_CHECKED_CONTRACT>135,"the Class transport reader excludes the stricter predecessor");
    FILE *file=fopen(XR_PATH_FIXTURES "/previous135-native-path/old135-path.chk","rb");CHECK(file);
    unsigned char *packet=xr_malloc(102525);CHECK(packet);size_t length=fread(packet,1,102525,file);
    CHECK(length==102524&&feof(file)&&!ferror(file)&&!fclose(file));
    static const uint8_t expected[32]={0x47,0x31,0xbb,0xb5,0xbf,0x81,0xd3,0x4e,0x03,0x24,0xc1,0x78,0x88,0x14,0x47,0x11,0x0b,0xb7,0xa7,0x5f,0xa5,0x50,0x72,0xe2,0xbb,0x5d,0xa2,0xf2,0xb5,0xcd,0xd5,0xb9};uint8_t digest[32];
    xr_sha256(packet,length,digest);CHECK(!memcmp(digest,expected,sizeof(digest)));
    CHECK(packet[8]==42&&packet[12]==135);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    uint32_t future=XR_XIR_CHECKED_CONTRACT+1;for(unsigned i=0;i<4;++i)packet[12+i]=(uint8_t)(future>>(i*8));
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    memset(packet,0xa5,length);xr_free(packet);
}
static XrXirStatus path_flavours_check(const XrXirCompileContext *context,const char *text,
    const XrXirLibraryCatalog *libraries,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic){
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"path-shadow",XR_PATH_FIXTURES};
    XrXirSourceRequest request={session,XR_PATH_FIXTURES "/client.xr",&authority,context,XR_PATH_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,libraries};
    XrXirSourceText input={"client.xr",text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void path_class_array_shape(const XrXirModule *module){
    uint32_t function=path_complete_named(module,"classArrayHandles"),array=UINT32_MAX;
    const XrXirFunction *body=module->functions+function;
    const XrXirTypeNode *input=body->parameter_count==1?xr_xir_type_node(module->types,body->parameters[0]):NULL;
    CHECK(input&&input->kind==XR_XIR_TYPE_SLICE&&input->element==XR_XIR_U8);
    bool later_length=false,later_cleanup=false;
    for(uint32_t i=0;i<body->instruction_count;++i){
        const XrXirInstruction *op=body->instructions+i;
        if(op->op==XR_XIR_ARRAY_NEW){
            const XrXirTypeNode *type=xr_xir_type_node(module->types,op->type);
            if(type&&type->kind==XR_XIR_TYPE_ARRAY&&xr_xir_type_is_class(module->types,type->element)){
                CHECK(array==UINT32_MAX&&op->args[1]==1);array=i;
            }
        }
        if(op->op==XR_XIR_VIEW_LEN&&op->args[0]==0){CHECK(array!=UINT32_MAX&&i>array);later_length=true;}
        if(op->op==XR_XIR_VIEW_CLEANUP)for(uint32_t p=0;p<op->args[1];++p)
            if(body->operands[op->args[0]+p]==0){CHECK(array!=UINT32_MAX&&i>array);later_cleanup=true;}
    }
    CHECK(array!=UINT32_MAX&&later_length&&later_cleanup);
}
static void path_class_array_refusals(const XrXirCompileContext *context){
    static const char *const texts[]={
        "final class BorrowedClass{data:Slice<u8> init(input:Slice<u8>){this.data=input}}\n",
        "import {Buffer} from mem\nfn bad()->i64{const owner=Buffer(3,true);const refs=[owner];const alias=refs[0];"
        "const view=owner.asBytes();alias.resize(4);return len(view)}\n"
    };
    for(unsigned n=0;n<2;++n){
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=path_flavours_check(context,texts[n],NULL,&result,&diagnostic);
        XrXirStatus expected=n?XR_XIR_UNRESOLVED:XR_XIR_BAD_TYPE;
        const char *reason=n?"(CALL)":"class field carrier is not implemented";
        bool located=diagnostic.module==0&&diagnostic.line==(n?2:1)&&strstr(diagnostic.message,reason);
        if(status!=expected||!located)fprintf(stderr,"Class array refusal%u status%u expected%u module%u %d:%d %s\n%s",
            n,status,expected,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message,texts[n]);
        CHECK(status==expected&&!result.checked&&!result.snapshot&&located);
        xr_xir_compile_source_result_free(&result);
    }
}
static void path_imported_index_contract(const XrXirCompileContext *context,const XrXirArtifact *cold){
    const XrXirModule *module=xr_xir_compile_artifact_module(cold);
    uint32_t function=path_complete_named(module,"_drive"),site=UINT32_MAX;
    const XrXirFunction *body=module->functions+function;
    const XrXirSourceModule *owner=module->declarations->modules+module->declarations->functions[function].module;
    static const char canonical[]="stdlib-module-v1:module=4:path:path=16:path/_lexical.xr";
    CHECK(owner->name_length==sizeof(canonical)-1&&!memcmp(owner->name,canonical,sizeof(canonical)-1));
    for(uint32_t i=0;i<body->instruction_count;++i)if(body->instructions[i].op==XR_XIR_STRING_INDEX_OF){
        CHECK(site==UINT32_MAX);site=i;
    }
    CHECK(site!=UINT32_MAX&&body->instructions[site].args[1]==3&&!body->instructions[site].immediate);
    const uint32_t *args=body->operands+body->instructions[site].args[0];
    CHECK(xr_xir_operand_type(body,args[0])==XR_XIR_STRING&&xr_xir_operand_type(body,args[1])==XR_XIR_STRING&&
        xr_xir_operand_type(body,args[2])==XR_XIR_I64&&body->instructions[site].type==XR_XIR_I64);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(cold);
    CHECK(xr_xir_compile_structure_verify_v2(context,module,construction,NULL)==XR_XIR_OK);
    XrXirFunction *functions=xr_malloc(module->function_count*sizeof(*functions));
    uint32_t *operands=xr_malloc(body->operand_count*sizeof(*operands));CHECK(functions&&operands);
    memcpy(functions,module->functions,module->function_count*sizeof(*functions));
    memcpy(operands,body->operands,body->operand_count*sizeof(*operands));
    XrXirModule changed=*module;changed.functions=functions;functions[function].operands=operands;
    uint32_t argument=body->instructions[site].args[0]+2,old=operands[argument];operands[argument]=args[0];
    XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_structure_verify_v2(context,&changed,construction,&diagnostic)==XR_XIR_BAD_TYPE&&
        diagnostic.function==function&&diagnostic.instruction==site);
    operands[argument]=old;CHECK(xr_xir_compile_structure_verify_v2(context,&changed,construction,NULL)==XR_XIR_OK);
    xr_free(operands);xr_free(functions);
}
static void path_imported_append_contract(const XrXirCompileContext *context,const XrXirArtifact *cold){
    const XrXirModule *module=xr_xir_compile_artifact_module(cold);
    static const char canonical[]="stdlib-module-v1:module=4:path:path=16:path/_lexical.xr";
    static const char name[]="normalize";uint32_t function=UINT32_MAX,site=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f){
        const XrXirFunction *body=module->functions+f;
        const XrXirSourceModule *owner=module->declarations->modules+module->declarations->functions[f].module;
        if(body->name_length==sizeof(name)-1&&!memcmp(body->name,name,sizeof(name)-1)&&
            owner->name_length==sizeof(canonical)-1&&!memcmp(owner->name,canonical,sizeof(canonical)-1)){
            CHECK(function==UINT32_MAX);function=f;
        }
    }
    CHECK(function!=UINT32_MAX);const XrXirFunction *body=module->functions+function;
    for(uint32_t i=0;i<body->instruction_count;++i)
        if(body->instructions[i].op==XR_XIR_CELL_STRING_APPEND&&!body->instructions[i].immediate){site=i;break;}
    CHECK(site!=UINT32_MAX);const XrXirInstruction *op=body->instructions+site;
    XrXirType cell=xr_xir_operand_type(body,op->args[0]);
    CHECK(op->type==XR_XIR_UNIT&&xr_xir_type_is_cell(module->types,cell)&&
        xr_xir_cell_element(module->types,cell)==XR_XIR_STRING&&xr_xir_operand_type(body,op->args[1])==XR_XIR_STRING);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(cold);
    CHECK(xr_xir_compile_structure_verify_v2(context,module,construction,NULL)==XR_XIR_OK);
    XrXirFunction *functions=xr_malloc(module->function_count*sizeof(*functions));
    XrXirInstruction *instructions=xr_malloc(body->instruction_count*sizeof(*instructions));CHECK(functions&&instructions);
    memcpy(functions,module->functions,module->function_count*sizeof(*functions));
    memcpy(instructions,body->instructions,body->instruction_count*sizeof(*instructions));
    XrXirModule changed=*module;changed.functions=functions;functions[function].instructions=instructions;
    uint32_t old=instructions[site].args[1];instructions[site].args[1]=instructions[site].args[0];
    XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_structure_verify_v2(context,&changed,construction,&diagnostic)==XR_XIR_BAD_TYPE&&
        diagnostic.function==function&&diagnostic.instruction==site);
    instructions[site].args[1]=old;
    CHECK(xr_xir_compile_structure_verify_v2(context,&changed,construction,NULL)==XR_XIR_OK);
    xr_free(instructions);xr_free(functions);
}
static void path_flavours_contracts(const XrXirCompileContext *context){
    static const char *const modules[]={"posix","windows","logical","logical"};
    for(unsigned n=0;n<4;++n){
        char text[1024];int count=snprintf(text,sizeof(text),
            "import io\nimport path\nimport \"std/path/%s\" as foreign\nfn good(){io.exists(path.from(\"x\"))}\n",modules[n]);
        CHECK(count>0&&(size_t)count<sizeof(text));
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(path_flavours_check(context,text,NULL,&result,&diagnostic)==XR_XIR_OK&&result.checked);
        xr_xir_compile_source_result_free(&result);
        const char *bad=n==3?"fn bad(){foreign.LogicalPath(\"unchecked\")}\n":
            n==2?"fn bad(){io.exists(foreign.from(\"x\")!)}\n":"fn bad(){io.exists(foreign.from(\"x\"))}\n";
        size_t suffix=strlen(bad);CHECK((size_t)count+suffix<sizeof(text));memcpy(text+count,bad,suffix+1);
        XrXirStatus status=path_flavours_check(context,text,NULL,&result,&diagnostic);
        const char *reason=n==3?"constructor arity or authority mismatch":"expression cannot satisfy its declared type";
        if(status!=XR_XIR_BAD_TYPE||diagnostic.module||diagnostic.line!=5||strcmp(diagnostic.message,reason))
            fprintf(stderr,"Path flavour refusal%u status%u module%u %d:%d %s\n",n,status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE&&!result.checked&&!result.snapshot);
        CHECK(!diagnostic.module&&diagnostic.line==5&&!strcmp(diagnostic.message,reason));
        xr_xir_compile_source_result_free(&result);
    }
}
static void path_flavours_product(const XrXirCompileContext *context,const char *emit_base){
    path_previous134(context);path_previous135(context);
    FILE *file=fopen(XR_PATH_FIXTURES "/flavours.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));
    long bytes=ftell(file);CHECK(bytes>0&&!fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)bytes+1);CHECK(text);
    CHECK(fread(text,1,(size_t)bytes,file)==(size_t)bytes&&!fclose(file));text[bytes]=0;
    XrXirLibraryCatalog *catalog=NULL;XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xir_stdlib_library_catalog_new(context,XR_PATH_ROOT "/stdlib",&catalog)==XR_XIR_OK);
    XrXirStatus status=path_flavours_check(context,text,catalog,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Path checked import status%u module%u %d:%d %s\n",
        status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK&&source.checked);xr_xir_compile_library_catalog_free(catalog);
    memset(text,0xa5,(size_t)bytes+1);xr_free(text);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    path_class_array_shape(xr_xir_compile_artifact_module(cold));path_imported_index_contract(context,cold);path_imported_append_contract(context,cold);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t ids[]={module->declarations->entry_function,path_complete_named(module,"check")};
    if(emit_base){
        char path[2048];int count=snprintf(path,sizeof(path),"%s.flavours.c",emit_base);CHECK(count>0&&(size_t)count<sizeof(path));
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(lowered,"path_flavours",16777216,&generated)==XR_XIR_OK);
        file=fopen(path,"wb");CHECK(file&&fwrite(generated.text,1,generated.length,file)==generated.length);
        CHECK(fprintf(file,"\nconst uint32_t path_flavours_ids[2]={%u,%u};\n",ids[0],ids[1])>0&&!fclose(file));
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(lowered);
    }else{
        path_flavours_contracts(context);path_class_array_refusals(context);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
        path_flavours_execute(program,ids);xr_xir_compile_program_drop(program);
    }
}
#endif // XIR_PATH_FLAVOURS_SOURCE_H
