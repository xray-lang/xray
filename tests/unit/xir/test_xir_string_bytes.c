/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_bytes.c - Complete string consumers after producer retirement
 *
 * KEY CONCEPT:
 *   Source emits the governed call and packet receivers own its borrowed types.
 */
#include "../../lir/lir_source_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_intrinsic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include "../test_win_compat.h"
#include "string_bytes_cases.h"

typedef struct StringBytesFixture {
    XrXirCompileContext context;
    XrXirArtifact *checked;
    uint8_t identity[32];
    StringBytesFunctions functions;
    uint32_t ascii;
} StringBytesFixture;

static uint32_t string_bytes_named(const XrXirModule *module,const char *name) {
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->functions[i].name_length==strlen(name) &&
            !memcmp(module->functions[i].name,name,strlen(name))) return i;
    CHECK(false);
    return 0;
}
static char *string_bytes_source(const char *path) {
    FILE *file=fopen(path,"rb");
    CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);
    CHECK(length>=0 && (uint64_t)length<SIZE_MAX && !fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)length+1);
    CHECK(text && fread(text,1,(size_t)length,file)==(size_t)length && !fclose(file));
    text[length]=0;
    return text;
}
static void string_bytes_registry(void) {
    const XrNativeIntrinsicDeclaration *call=xr_xir_intrinsic(4);
    CHECK(call && call->member->id==18 && !call->ctfe);
    CHECK(call->implementation==XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_BYTES);
    CHECK(call->member->ownership==XR_NATIVE_RESULT_OWNERSHIP_BORROWED);
    CHECK(call->member->allocation==XR_NATIVE_ALLOCATION_MAY_HEAP);
    CHECK(call->member->result==XR_NATIVE_TERM_SLICE_U8);
    CHECK(!xr_xir_intrinsic(5) && !xr_xir_intrinsic(6));
}
static void string_bytes_fixture_names(StringBytesFixture *fixture) {
    const XrXirModule *module=xr_xir_compile_artifact_module(fixture->checked);
    fixture->functions=(StringBytesFunctions){module->declarations->entry_function,
        string_bytes_named(module,"printableName"),string_bytes_named(module,"cStringName"),
        string_bytes_named(module,"discardBytes"),string_bytes_named(module,"scalarBytes"),
        string_bytes_named(module,"refAndPanic")};
    fixture->ascii=string_bytes_named(module,"_asciiPrintable");
}
static StringBytesFixture string_bytes_fixture(const char *path) {
    char *text=string_bytes_source(path);
    SourceFixture source=source_fixture(text);
    memset(text,0xa5,strlen(text));
    xr_free(text);
    StringBytesFixture fixture={0};
    fixture.context=*xr_xir_compile_artifact_context(source.closed);
    CHECK(xr_compile_resources_retain(fixture.context.resources)==XR_COMPILE_RESOURCE_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.closed);
    bool found=false;
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
            const XrXirFunction *body=module->functions+f;
            const XrXirInstruction *op=body->instructions+i;
            CHECK(op->op!=XR_XIR_VIEW_NEW);
            if (!xr_xir_intrinsic_string_bytes(op)) continue;
            CHECK(op->args[1]==1);
            CHECK(xr_xir_intrinsic_string_bytes_types(module->types,
                xr_xir_operand_type(body,body->operands[op->args[0]]),op->type));
            found=true;
        }
    CHECK(found);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source.closed,&packet,NULL)==XR_XIR_OK);
    xr_sha256(packet.bytes,packet.length,fixture.identity);
    source_fixture_free(&source);
    CHECK(xr_xir_compile_checked_read(&fixture.context,packet.bytes,packet.length,
        &fixture.checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    string_bytes_fixture_names(&fixture);
    return fixture;
}
static StringBytesFixture string_bytes_packet_fixture(const char *path) {
    FILE *file=fopen(path,"rb");
    CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);
    CHECK(length>0 && (uint64_t)length<=SIZE_MAX && !fseek(file,0,SEEK_SET));
    uint8_t *bytes=xr_malloc((size_t)length);
    CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    StringBytesFixture fixture={0};
    const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&fixture.context.resources)==XR_COMPILE_RESOURCE_OK);
    fixture.context.limits=xr_xir_compile_default_limits();
    xr_sha256(bytes,(size_t)length,fixture.identity);
    CHECK(xr_xir_compile_checked_read(&fixture.context,bytes,(size_t)length,
        &fixture.checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)length);
    xr_free(bytes);
    string_bytes_fixture_names(&fixture);
    return fixture;
}
static void string_bytes_fixture_free(StringBytesFixture *fixture) {
    xr_xir_compile_artifact_free(fixture->checked);
    xr_compile_resources_release(fixture->context.resources);
    *fixture=(StringBytesFixture){0};
}
static void string_bytes_source_checked(const XrXirSourceRequest *request,const char *name,
    const char *text,XrXirSourceResult *result) {
    XrXirSourceText input={name,text,strlen(text)};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(request,&input,result,&diagnostic,NULL);
    if (status!=XR_XIR_OK)
        fprintf(stderr,"string bytes catalog %s %u:%d:%d %s\n",name,(unsigned)status,
            diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result->checked && result->snapshot);
}
static StringBytesFixture string_bytes_catalog_fixture(const char *path) {
    StringBytesFixture fixture={0};
    const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&fixture.context.resources)==XR_COMPILE_RESOURCE_OK);
    fixture.context.limits=xr_xir_compile_default_limits();
    char directory[XR_TEST_PATH_MAX]="string-bytes-catalog-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX],library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    int length=snprintf(library,sizeof(library),"%s/lib.xr",absolute);
    CHECK(length>0 && (size_t)length<sizeof(library));
    length=snprintf(root,sizeof(root),"%s/root.xr",absolute);
    CHECK(length>0 && (size_t)length<sizeof(root));
    char *text=string_bytes_source(path);
    char *body=strstr(text,"// TEST-CONSUMER");
    CHECK(body);
    const char imports[]="import {printableName,cStringName,discardBytes,scalarBytes} from \"./lib\"\n"
        "fn caller0()->i64{return 11}\nfn caller1()->i64{return 13}\n";
    size_t consumer_size=sizeof(imports)+strlen(body);
    char *consumer=xr_malloc(consumer_size);
    CHECK(consumer);
    memcpy(consumer,imports,sizeof(imports)-1);
    memcpy(consumer+sizeof(imports)-1,body,strlen(body)+1);
    *body=0;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(fixture.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={.session=session,.entry_path=library,.authority=&authority,
        .context=&fixture.context,.linkage_kind=XR_XIR_LIBRARY};
    XrXirSourceResult produced={0};
    string_bytes_source_checked(&request,"lib.xr",text,&produced);
    uint32_t original=string_bytes_named(xr_xir_compile_artifact_module(produced.checked),"_asciiPrintable");
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(produced.checked,&packet,NULL)==XR_XIR_OK);
    memset(text,0xa5,(size_t)(body-text));
    xr_free(text);
    xr_xir_compile_source_result_free(&produced);
    xr_compile_session_free(session);
    session=NULL;
    XrXirLibraryModuleInput binding={authority,"lib.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&fixture.context,&input,1,&catalog);
    if (status!=XR_XIR_OK) fprintf(stderr,"string bytes Catalog status=%u\n",(unsigned)status);
    CHECK(status==XR_XIR_OK && catalog);
    memset(packet.bytes,0xa5,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(fixture.context.resources,&session)==XR_COMPILER_SESSION_OK);
    request.session=session;
    request.entry_path=root;
    request.linkage_kind=XR_XIR_PROGRAM;
    request.libraries=catalog;
    XrXirSourceResult received={0};
    string_bytes_source_checked(&request,"root.xr",consumer,&received);
    memset(consumer,0xa5,consumer_size);
    xr_free(consumer);
    xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_specialize(received.checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    xr_sha256(packet.bytes,packet.length,fixture.identity);
    CHECK(xr_xir_compile_checked_read(&fixture.context,packet.bytes,packet.length,
        &fixture.checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    string_bytes_fixture_names(&fixture);
    CHECK(fixture.ascii!=original);
    CHECK(xr_test_rmdir(directory)==0);
    memset(absolute,0xa5,sizeof(absolute));
    memset(library,0xa5,sizeof(library));
    memset(root,0xa5,sizeof(root));
    return fixture;
}
static void string_bytes_put32(uint8_t *bytes,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(value>>(i*8));
}
static void string_bytes_packet_rejected(const StringBytesFixture *fixture,
    const XrXirCheckedPacket *packet,size_t offset,uint32_t value,XrXirStatus expected) {
    CHECK(offset<=packet->length-4);
    uint8_t *bytes=xr_malloc(packet->length);
    CHECK(bytes);
    memcpy(bytes,packet->bytes,packet->length);
    string_bytes_put32(bytes+offset,value);
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash,bytes,32);
    xr_sha256_update(&hash,bytes+64,packet->length-64);
    xr_sha256_final(&hash,bytes+32);
    XrXirArtifact *decoded=NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(&fixture->context,bytes,packet->length,&decoded,&diagnostic);
    if (status!=expected)
        fprintf(stderr,"string bytes packet offset=%zu value=%u status=%u expected=%u\n",
            offset,value,(unsigned)status,(unsigned)expected);
    CHECK(status==expected && !decoded);
    memset(bytes,0xa5,packet->length);
    xr_free(bytes);
}
static void string_bytes_invalid_packets(const StringBytesFixture *fixture) {
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(fixture->checked,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(fixture->checked);
    const XrXirFunction *body=module->functions+fixture->ascii;
    CHECK(body->parameter_count && body->parameters[0]==XR_XIR_STRING);
    uint32_t instruction=0;
    while (instruction<body->instruction_count && !xr_xir_intrinsic_string_bytes(body->instructions+instruction))
        ++instruction;
    CHECK(instruction<body->instruction_count);
    const XrXirInstruction *op=body->instructions+instruction;
    size_t wire_size=(size_t)body->instruction_count*40;
    uint8_t *wire=xr_malloc(wire_size);
    CHECK(wire);
    for (uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *encoded=body->instructions+i;
        uint32_t words[]={encoded->op,encoded->type,encoded->args[0],encoded->args[1],
            encoded->targets[0],encoded->targets[1],(uint32_t)encoded->immediate,
            (uint32_t)((uint64_t)encoded->immediate>>32),encoded->type_arguments[0],encoded->type_arguments[1]};
        for (size_t w=0;w<sizeof(words)/sizeof(*words);++w)
            string_bytes_put32(wire+(size_t)i*40+w*4,words[w]);
    }
    size_t begin=64;
    while (begin+wire_size<=packet.length && memcmp(packet.bytes+begin,wire,wire_size)) ++begin;
    CHECK(begin+wire_size<=packet.length);
    xr_free(wire);
    size_t at=begin+(size_t)instruction*40;
    size_t operands=begin+wire_size+4;
    CHECK(operands<=packet.length && body->operand_count<=(packet.length-operands)/4);
    for (uint32_t i=0;i<body->operand_count;++i) {
        uint8_t encoded[4];string_bytes_put32(encoded,body->operands[i]);
        CHECK(!memcmp(packet.bytes+operands+(size_t)i*4,encoded,4));
    }
    string_bytes_packet_rejected(fixture,&packet,at+24,0,XR_XIR_BAD_STRUCTURE);
    string_bytes_packet_rejected(fixture,&packet,at+24,5,XR_XIR_BAD_STRUCTURE);
    string_bytes_packet_rejected(fixture,&packet,at+24,6,XR_XIR_BAD_STRUCTURE);
    string_bytes_packet_rejected(fixture,&packet,at+28,1,XR_XIR_BAD_STRUCTURE);
    string_bytes_packet_rejected(fixture,&packet,at+12,0,XR_XIR_BAD_STRUCTURE);
    string_bytes_packet_rejected(fixture,&packet,at+4,XR_XIR_BOOL,XR_XIR_BAD_TYPE);
    string_bytes_packet_rejected(fixture,&packet,operands+(size_t)op->args[0]*4,0,XR_XIR_BAD_TYPE);
    string_bytes_packet_rejected(fixture,&packet,12,98,XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_checked_packet_free(&packet);
}
static XrXirArtifact *string_bytes_lowered(const StringBytesFixture *fixture) {
    XrXirArtifact *code=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(fixture->checked,&target,&code,NULL)==XR_XIR_OK);
    return code;
}
static void string_bytes_ctfe(const StringBytesFixture *fixture,const XrXirArtifact *code) {
    XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(XR_RESOURCE_DEFAULT_LIMIT,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};
    CHECK(xr_xir_string_new(domain,"a",1,&argument)==XR_XIR_VALUE_OK);
    XrXirCtfeRequest request={.artifact=code,.function=fixture->ascii,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=&argument,.argument_count=1,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity,fixture->identity,sizeof(fixture->identity));
    CHECK(xr_xir_compile_ctfe_leaf(&request,&result)==XR_XIR_CTFE_NOT_LEAF);
    xr_xir_value_drop(&argument);
    xr_xir_domain_drop(domain);
}
static void string_bytes_source_rejected(const char *text,bool borrowed) {
    const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"string-bytes-negative",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={NULL,text,strlen(text)};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    bool semantic=status==XR_XIR_BAD_TYPE || status==XR_XIR_BAD_VALUE;
    bool lifetime=borrowed && status==XR_XIR_UNRESOLVED &&
        strstr(diagnostic.message,"view lifetime normalization") &&
        strstr(diagnostic.message,"(CALL_INTRINSIC)");
    fprintf(stderr,"string bytes rejected status=%u source=%s\n",(unsigned)status,text);
    if (!semantic && !lifetime)
        fprintf(stderr,"string bytes rejection at %d:%d %s\n",
            diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK((semantic || lifetime) && diagnostic.status==status && !result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
}
static void string_bytes_emit(StringBytesFixture *fixture,const char *path,const char *packet_path) {
    XrXirArtifact *code=string_bytes_lowered(fixture);
    XrXirCSource generated={0};
    CHECK(xr_xir_compile_emit_c(code,"string_bytes",XR_RESOURCE_DEFAULT_SIZE_LIMIT,&generated)==XR_XIR_OK);
    CHECK(generated.length && strstr(generated.text,"xr_xir_call_view_operation"));
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(fixture->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(code);
    string_bytes_fixture_free(fixture);
    FILE *file=fopen(path,"wb");
    CHECK(file && fwrite(generated.text,1,generated.length,file)==generated.length && !fclose(file));
    file=fopen(packet_path,"wb");
    CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_c_source_free(&generated);
}
static void string_bytes_vm(StringBytesFixture *fixture) {
    XrXirArtifact *code=string_bytes_lowered(fixture);
    string_bytes_ctfe(fixture,code);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
    StringBytesFunctions functions=fixture->functions;
    string_bytes_fixture_free(fixture);
    string_bytes_run(program,&functions);
    xr_xir_compile_program_drop(program);
}
int main(int argc,char **argv) {
    const bool emit=argc==5 && !strcmp(argv[1],"--emit");
    const bool packet=argc==3 && !strcmp(argv[1],"--packet");
    CHECK(emit || packet || argc==2);
    StringBytesFixture fixture=packet?string_bytes_packet_fixture(argv[2]):
        emit?string_bytes_catalog_fixture(argv[2]):string_bytes_fixture(argv[1]);
    if (emit) {
        string_bytes_emit(&fixture,argv[3],argv[4]);
        return 0;
    }
    string_bytes_registry();
    string_bytes_source_rejected("fn bad()->i64{return len(\"temporary\".bytes())}",false);
    string_bytes_source_rejected("fn bad(s:string)->Slice<u8>{return s.bytes()}",true);
    string_bytes_source_rejected("fn bad(s:ref string)->i64{const b=s.bytes();s=\"changed\";return len(b)}",true);
    string_bytes_source_rejected("fn bad()->i64{var s=\"a\";const b=s.bytes();s=\"changed\";return len(b)}",true);
    string_bytes_invalid_packets(&fixture);
    string_bytes_vm(&fixture);
    if (!packet) {
        fixture=string_bytes_catalog_fixture(argv[1]);
        string_bytes_invalid_packets(&fixture);
        string_bytes_vm(&fixture);
    }
    puts("governed string bytes: complete helpers, real callers, cold Catalog and retired packet owners PASS");
    return 0;
}
