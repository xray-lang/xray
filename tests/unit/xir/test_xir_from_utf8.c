/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_from_utf8.c - Source-created throwing native callable lifecycle
 */
#if defined(XR_FROM_UTF8_VALUE_OBSERVER)
#include "base/xmalloc.h"
#include "xir/xxir_compile_domain.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fromUtf8 physical %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t fault_index=SIZE_MAX,fault_calls,fault_live,fault_conversions;
static bool fault_active;
static XrXirDomain *fault_domain;
static bool fault_refuse(void) {return fault_active && fault_calls++==fault_index && fault_index<2;}
static void *fault_malloc(size_t size) {if(fault_refuse())return NULL;void *p=xr_malloc(size);if(p)++fault_live;return p;}
static void *fault_calloc(size_t count,size_t size) {if(fault_refuse())return NULL;void *p=xr_calloc(count,size);if(p)++fault_live;return p;}
static void *fault_realloc(void *p,size_t size) {if(fault_refuse())return NULL;bool empty=!p;void *q=xr_realloc(p,size);if(q && empty)++fault_live;return q;}
static void fault_free(void *p) {if(p){CHECK(fault_live);--fault_live;}xr_free(p);}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) fault_malloc(bytes)
#define xr_calloc(count,bytes) fault_calloc(count,bytes)
#define xr_realloc(p,bytes) fault_realloc(p,bytes)
#define xr_free(p) fault_free(p)
#define xir_from_utf8 xir_observed_from_utf8
#include "xir/xxir_value.c"
#undef xir_from_utf8
XR_FUNC XrXirValueStatus xir_from_utf8(const XrXirValue *input,const XrXirViewAccess *access,XrXirValue *output) {
    CHECK(!fault_domain && xr_xir_domain_retain(access->values->domain));fault_domain=access->values->domain;
    size_t live=fault_live;uint64_t held=xr_xir_domain_stats(access->values->domain).live_bytes;fault_calls=0;fault_active=true;++fault_conversions;
    XrXirValueStatus status=xir_observed_from_utf8(input,access,output);fault_active=false;
    if (fault_index==2) {
        CHECK(status==XR_XIR_VALUE_OK && output->type==XR_XIR_STRING && fault_calls==2);
        CHECK(xr_xir_call_request_cancel(access->context)==XR_XIR_CALL_CANCEL_REQUESTED);
    } else CHECK(status==XR_XIR_VALUE_OOM && !output->type && !output->reserved && !output->payload && live==fault_live && xr_xir_domain_stats(access->values->domain).live_bytes==held);
    return status;
}
void xr_test_from_utf8_fault_begin(size_t index) {CHECK(index<=2);fault_index=index;fault_conversions=0;}
void xr_test_from_utf8_fault_finish(size_t index) {
    CHECK(fault_conversions==1 && fault_calls==(index<2?index+1:2) && fault_domain);
    XrXirDomainStats stats=xr_xir_domain_stats(fault_domain);CHECK(stats.allocations==stats.frees+1);
    xr_xir_domain_drop(fault_domain);fault_domain=NULL;CHECK(!fault_live);
    printf("Strict fromUtf8 actual String resource case %zu, terminal owner rollback PASS\n",index);
}
#else
#include "xray_source_entry.h"
#include "program/xr_xir_source_product.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_nominal.h"
#include "shared/xnative_declaration.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_output.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_instance_value.h"
#include "base/xsha256.h"
#include "base/xio_policy.h"
#include "../test_win_compat.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "from_utf8_cases.h"
#if defined(XR_FROM_UTF8_WITH_OBSERVER)
void xr_test_from_utf8_fault_begin(size_t);
void xr_test_from_utf8_fault_finish(size_t);
#endif
typedef struct Output { char bytes[4096];size_t length; } Output;
static XrSourceWriteStatus write_bytes(void *context,XrSourceOutputStream stream,const char *text,size_t length) {
    Output *output=context;CHECK(stream==XR_SOURCE_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,text,length);output->length+=length;return XR_SOURCE_WRITE_OK;
}
static struct {int cancelled;size_t polls,finished;} compile_cancel;
static int compile_cancel_poll(const void *context) {
    (void)context;++compile_cancel.polls;return compile_cancel.cancelled;
}
static XrSourceArtifact *build_text(XrSourceTarget target,const char *text,size_t length) {
    XrSourceCompileOptions options=xray_source_compile_options_default();options.target=target;options.stdlib_path=XR_PUBLIC_STDLIB;
    compile_cancel.cancelled=0;compile_cancel.polls=0;options.cancel_poll=compile_cancel_poll;options.cancel_context=&compile_cancel;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"strict-from-utf8",text,length}};
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    if (diagnostic) {XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
        fprintf(stderr,"Source %u stage=%u %d:%d %s\n",(unsigned)status,(unsigned)failure.stage,info.primary.line,info.primary.column,info.message);}
    xray_source_diagnostic_free(diagnostic);CHECK(status==XR_SOURCE_OK && artifact);
    compile_cancel.finished=compile_cancel.polls;compile_cancel.cancelled=1;return artifact;
}
static void identity_truncation(const XrXirCompileContext *context,const XrXirSourceProductPacketView *packet,const XrXirModule *module) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==39,"this prefix test describes current wire39");
    size_t offset=64+12;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=module->functions+f;
        uint64_t bytes=28u+(uint64_t)body->name_length+4u*(uint64_t)body->parameter_count+
            16u*(uint64_t)body->block_count+40u*(uint64_t)body->instruction_count+4u*(uint64_t)body->operand_count;
        CHECK(offset<=packet->length && bytes<=packet->length-offset);offset+=(size_t)bytes;
    }
    CHECK(packet->length-offset>=20);offset+=20;
    for (uint32_t m=0;m<module->declarations->module_count;++m) {
        const XrXirSourceModule *row=module->declarations->modules+m;
        uint64_t bytes=20u+(uint64_t)row->name_length+4u*(uint64_t)row->dependency_count;
        CHECK(offset<=packet->length && bytes<=packet->length-offset);offset+=(size_t)bytes;
    }
    size_t words=(size_t)module->function_count*40;CHECK(words && words<=packet->length-offset);
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunctionIdentity *id=module->declarations->functions+f;
        const uint32_t fields[]={id->module,id->exported,id->nominal_owner,id->member_access,id->cleanup_owner,
            id->promises,id->method_kind,id->test_role,id->test_timeout_seconds,id->native_callable};
        for (size_t field=0;field<10;++field) for(unsigned byte=0;byte<4;++byte)
            CHECK(packet->bytes[offset+(size_t)f*40+field*4+byte]==(uint8_t)(fields[field]>>(8*byte)));
    }
    CHECK(module->declarations->functions[module->function_count-1].native_callable);
    size_t length=offset+words-4;uint8_t *truncated=malloc(length);CHECK(truncated);memcpy(truncated,packet->bytes,length);
    for(unsigned byte=0;byte<8;++byte) truncated[24+byte]=(uint8_t)((uint64_t)(length-64)>>(8*byte));
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,truncated,32);xr_sha256_update(&hash,truncated+64,length-64);
    xr_sha256_final(&hash,truncated+32);uint64_t peak=0;
    for(unsigned finite=0;finite<2;++finite) {
        XrCompileResourceLimits limits={finite?XR_COMPILE_MEMORY_FINITE:XR_COMPILE_MEMORY_ORDINARY,finite?peak:0,true};
        XrXirCompileContext receiver={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&receiver.resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(receiver.resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(&receiver,truncated,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(xr_compile_resources_stats(receiver.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
        if(!finite) peak=after.peak_bytes;xr_compile_resources_release(receiver.resources);
    }
    XrXirArtifact *restored=NULL;CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&restored,NULL)==XR_XIR_OK && restored);
    xr_xir_compile_artifact_free(restored);free(truncated);
    puts("Wire39 native identity tenth-field truncation precedes finite allocation refusal and preserves output PASS");
}
static void identity_checks(const XrXirCompileContext *context,const XrXirSourceProduct *product) {
    XrXirSourceProductPacketView packet={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK && closed);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(closed);
    const XrXirModule *source=xr_xir_compile_artifact_module(closed);CHECK(source && source->declarations);
    identity_truncation(context,&packet,source);
    XrXirModule module=*source;XrXirDeclarations declarations=*source->declarations;
    XrXirFunction *functions=malloc((size_t)source->function_count*sizeof(*functions));
    XrXirFunctionIdentity *identities=malloc((size_t)source->function_count*sizeof(*identities));CHECK(functions && identities);
    memcpy(functions,source->functions,(size_t)source->function_count*sizeof(*functions));
    memcpy(identities,source->declarations->functions,(size_t)source->function_count*sizeof(*identities));
    declarations.functions=identities;module.functions=functions;module.declarations=&declarations;
    XrXirDiagnostic diagnostic={0};uint32_t native=UINT32_MAX;
    for (uint32_t f=0;f<module.function_count;++f) if (identities[f].native_callable) {CHECK(native==UINT32_MAX);native=f;}
    CHECK(native!=UINT32_MAX);
    CHECK(xr_xir_compile_ctfe_type_admit(context,module.types,functions[native].parameters[0])==XR_XIR_CTFE_NOT_SUPPORTED);
    XrXirStatus status=xr_xir_compile_verify_v2(context,&module,construction,&diagnostic);
    fprintf(stderr,"Identity baseline=%u f=%u b=%u i=%u\n",(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    XrXirFunction saved=functions[native];XrXirFunctionIdentity identity=identities[native];
    identities[native].native_callable=UINT32_MAX;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    identities[native]=identity;functions[native].error_type=XR_XIR_NEVER;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_TYPE);
    functions[native]=saved;functions[native].error_type=XR_XIR_ERROR;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_TYPE);
    functions[native]=saved;XrXirInstruction instruction={.op=XR_XIR_RETURN};XrXirBlock block={.first=0,.count=1};
    functions[native].blocks=&block;functions[native].block_count=1;
    functions[native].instructions=&instruction;functions[native].instruction_count=1;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    functions[native]=saved;identities[native].native_callable=0;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    identities[native]=identity;functions[native].name="x\0y";functions[native].name_length=3;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_STRUCTURE);functions[native]=saved;
    const XrXirTypes *original_types=module.types;XrXirTypes types=*module.types;XrXirNominalTable nominals=*types.nominals;
    XrXirNominalDeclaration *records=malloc((size_t)nominals.count*sizeof(*records));CHECK(records && nominals.declarations);
    memcpy(records,nominals.declarations,(size_t)nominals.count*sizeof(*records));nominals.declarations=records;types.nominals=&nominals;module.types=&types;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_OK);uint32_t error=UINT32_MAX;
    for(uint32_t n=0;n<nominals.count;++n) if(records[n].native.native_id==XR_NATIVE_DECLARATION_UTF8_ERROR) error=n;
    CHECK(error!=UINT32_MAX);records[error].native.source_fingerprint[0]^=1;
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    records[error].native.source_fingerprint[0]^=1;CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_OK);
    module.types=original_types;free(records);
    CHECK(xr_xir_compile_verify_v2(context,&module,construction,&diagnostic)==XR_XIR_OK);
    free(identities);free(functions);xr_xir_compile_artifact_free(closed);
    puts("Strict fromUtf8 canonical identity/signature/empty-body rejection PASS");
}
static int product_check(const char *text,size_t length,bool identities,
    const XrCompileResourceLimits *provided,uint64_t *peak,XrXirStatus *observed,const char *rejection) {
    XrCompileResourceLimits limits=provided?*provided:(XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"from-utf8-product",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,&context,XR_PUBLIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceText input={NULL,text,length};XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};XrFrontendRoot diagnostic_root={.message=""};
    XrXirStatus status=xr_xir_compile_source_product_build_text(&request,&input,&product,&diagnostic);
    diagnostic_root=(XrFrontendRoot){.message=""};
    if(diagnostic.failure.report) {
        XrFrontendReportInfo info={0};CHECK(xr_frontend_report_info(diagnostic.failure.report,&info)&&info.roots==1);
        CHECK(xr_frontend_report_root(diagnostic.failure.report,0,&diagnostic_root));
    }
    xr_compile_session_free(session);
    CHECK(status==XR_XIR_OK || !product);
    fprintf(stderr,"Product status=%u stage=%u f=%u b=%u i=%u reason=%u source=%s\n",(unsigned)status,
        (unsigned)diagnostic.stage,diagnostic.xir.function,diagnostic.xir.block,diagnostic.xir.instruction,
        (unsigned)diagnostic.xir.reason,diagnostic_root.message);
    if (rejection) CHECK(status==XR_XIR_BAD_TYPE && diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CHECK &&
        !product && !strcmp(diagnostic_root.message,rejection));
    if (identities) {CHECK(status==XR_XIR_OK && product);identity_checks(&context,product);}
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    if (peak) *peak=after.peak_bytes;if (observed) *observed=status;
    xr_compile_resources_release(context.resources);return status==XR_XIR_OK?0:1;
}
static void held_checks(void) {
    uint64_t peak=0;XrXirStatus status=XR_XIR_OK;
    CHECK(!product_check(from_utf8_owned_source,sizeof(from_utf8_owned_source)-1,false,NULL,&peak,&status,NULL));
    CHECK(peak>1 && status==XR_XIR_OK);
    for (int difference=-1;difference<=1;++difference) {
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,peak+(uint64_t)difference,true};
        int failed=product_check(from_utf8_owned_source,sizeof(from_utf8_owned_source)-1,false,&limits,NULL,&status,NULL);
        CHECK(status==(difference<0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK) && failed==(difference<0));
    }
    printf("Strict fromUtf8 full Source/product held %llu +/-1 with retired owner PASS\n",(unsigned long long)peak);
}
static void source_rejections(void) {
    const char no_try[]="fn decode(bytes:Slice<u8>)->string { return string.fromUtf8(bytes) }\n";
    const char ctfe[]="fn decode() throws(Utf8Error)->string { return try comptime { var bytes:Array<u8>=[65]; try string.fromUtf8(bytes[:]) } }\nprint(try decode())\n";
    XrXirStatus status=XR_XIR_OK;
    CHECK(product_check(no_try,sizeof(no_try)-1,false,NULL,NULL,&status,"E0330: throwing call requires try or a try body") && status==XR_XIR_BAD_TYPE);
    CHECK(!product_check(ctfe,sizeof(ctfe)-1,false,NULL,NULL,&status,NULL) && status==XR_XIR_OK);
    puts("Strict fromUtf8 no-try rejection and compiler-owned Slice conversion without held leak PASS");
}
static void historical_check(const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long count=ftell(file);
    CHECK(count>0 && !fseek(file,0,SEEK_SET));unsigned char *bytes=malloc((size_t)count);CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)count,file)==(size_t)count && !fclose(file));
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};XrXirArtifact *output=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_checked_read(&context,bytes,(size_t)count,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);free(bytes);puts("Authentic old wire38/common99 packet rejected without publication PASS");
}
static XrXirOutputStatus library_bytes(void *user,XrXirOutputStream stream,const char *text,size_t bytes) {
    Output *output=user;CHECK(stream==XR_XIR_STDOUT && bytes<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,text,bytes);output->length+=bytes;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus library_group(void *user,const XrXirOutputGroup *group) {return xr_xir_output_render(user,group);}
static void library_check(const char *emit) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    char directory[XR_TEST_PATH_MAX]="from-utf8-library-XXXXXX",absolute[XR_TEST_PATH_MAX];
    char library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];CHECK(xr_test_mkdtemp(directory));
    CHECK(xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    int size=snprintf(library,sizeof(library),"%s/lib.xr",absolute);CHECK(size>0 && (size_t)size<sizeof(library));
    size=snprintf(root,sizeof(root),"%s/root.xr",absolute);CHECK(size>0 && (size_t)size<sizeof(root));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={.session=session,.entry_path=library,.authority=&authority,.context=&context,
        .stdlib_path=XR_PUBLIC_STDLIB,.linkage_kind=XR_XIR_LIBRARY};
    char source[]="export fn decode(bytes:Slice<u8>) throws(Utf8Error)->string { return try string.fromUtf8(bytes) }\n";
    XrXirSourceText input={"lib.xr",source,sizeof(source)-1};XrXirSourceResult produced={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&produced,&diagnostic,NULL);
    fprintf(stderr,"Library Source status=%u %s\n",(unsigned)status,diagnostic.message);CHECK(status==XR_XIR_OK && produced.checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(produced.checked,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(produced.checked);
    CHECK(module->declarations->module_count==2);XrXirLibraryModuleInput bindings[2];
    char *library_identity=NULL;CHECK(xr_compile_module_identity_from_logical(context.resources,&authority,"lib.xr",&library_identity)==XR_MODULE_OK);
    uint32_t matched=0;XrOsIoPolicy policy=xr_compile_io_policy(context.resources);char *physical_stdlib=NULL;
    CHECK(xr_realpath_owned(&policy,XR_PUBLIC_STDLIB,&physical_stdlib)==XR_OS_IO_OK);
    for (uint32_t m=0;m<2;++m) {
        fprintf(stderr,"Library module %u %.*s\n",m,(int)module->declarations->modules[m].name_length,module->declarations->modules[m].name);
        bool script=module->declarations->modules[m].name_length==strlen(library_identity) &&
            !memcmp(module->declarations->modules[m].name,library_identity,strlen(library_identity));
        matched+=script;
        bindings[m]=script?(XrXirLibraryModuleInput){authority,"lib.xr"}:
            (XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_STDLIB,"prelude",physical_stdlib},"prelude/builtin_symbols.def"};
    }
    CHECK(matched==1);xr_compile_resources_free(library_identity);
    XrXirLibraryInput admitted={packet.bytes,packet.length,{0},bindings,2};
    xr_sha256(packet.bytes,packet.length,admitted.sha256);
    XrXirArtifact *decoded=NULL;status=xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,NULL);
    fprintf(stderr,"Library packet reread status=%u\n",(unsigned)status);CHECK(status==XR_XIR_OK && decoded);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_source_result_free(&produced);xr_compile_session_free(session);session=NULL;memset(source,0xa5,sizeof(source));
    XrXirLibraryCatalog *catalog=NULL;status=xr_xir_compile_library_catalog_new_v2(&context,&admitted,1,&catalog);
    fprintf(stderr,"Library Catalog status=%u\n",(unsigned)status);CHECK(status==XR_XIR_OK && catalog);xr_compile_resources_free(physical_stdlib);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    request.session=session;request.entry_path=root;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
    char consumer[]="import {decode} from \"./lib\"\nfn check() {var good:Array<u8>=[65,0,195,169];try { print(try decode(good[:])) } catch(e:Utf8Error) {print(\"unexpected\")}\nvar bad:Array<u8>=[255];try { print(try decode(bad[:])) } catch(e:Utf8Error) { print(\"invalid\") }}\ncheck()\n";
    input=(XrXirSourceText){"root.xr",consumer,sizeof(consumer)-1};XrXirSourceProductRequest product_request={request,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic failure={0};XrFrontendRoot failure_root={.message=""};
    status=xr_xir_compile_source_product_build_text(&product_request,&input,&product,&failure);
    failure_root=(XrFrontendRoot){.message=""};
    if(failure.failure.report) {
        XrFrontendReportInfo info={0};CHECK(xr_frontend_report_info(failure.failure.report,&info)&&info.roots==1);
        CHECK(xr_frontend_report_root(failure.failure.report,0,&failure_root));
    }
    fprintf(stderr,"Library consumer status=%u stage=%u f=%u i=%u %s\n",(unsigned)status,(unsigned)failure.stage,failure.xir.function,failure.xir.instruction,failure_root.message);
    CHECK(status==XR_XIR_OK && product);uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);memset(consumer,0xa5,sizeof(consumer));
    CHECK(xr_test_rmdir(directory)==0);memset(absolute,0xa5,sizeof(absolute));
    if (emit) {XrXirCSource code={0};CHECK(xr_xir_compile_source_product_emit(product,"source_artifact",SIZE_MAX,&code)==XR_XIR_OK);
        FILE *file=fopen(emit,"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length && !fclose(file));xr_xir_compile_c_source_free(&code);}
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK && program);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&failure);
    Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,library_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,library_group,&sink};XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && xr_xir_value_valid(&result));xr_xir_value_drop(&result);
    CHECK(output.length==sizeof(from_utf8_expected)-1 && !memcmp(output.bytes,from_utf8_expected,output.length));
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);puts("Strict fromUtf8 Source/Library packet and retired Catalog producer VM PASS");
}
typedef struct GuardProbe {uint32_t bytes,calls,releases;} GuardProbe;
static const uint64_t guard_marker=UINT64_C(0x1122334400000003);
static XrXirAction guard_probe(XrXirCallView *view) {
    GuardProbe *probe=(GuardProbe *)view->environment;++probe->calls;
    if (probe->bytes) memcpy(view->state,&guard_marker,sizeof(guard_marker));
    XrXirAction rejected=xr_xir_instance_native_callable(view);
    CHECK(rejected.kind==XR_XIR_ACTION_FAULT && rejected.value.type==XR_XIR_I64 && rejected.value.payload==XR_XIR_CALL_BAD_STATE);
    if (probe->bytes) CHECK(!memcmp(view->state,&guard_marker,sizeof(guard_marker)));
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,42}};
}
static void guard_release(XrXirCallView *view,XrXirCallStatus reason) {
    GuardProbe *probe=(GuardProbe *)view->environment;CHECK(reason==XR_XIR_CALL_RETURNED);++probe->releases;
    if (probe->bytes) CHECK(!memcmp(view->state,&guard_marker,sizeof(guard_marker)));
}
static void guard_checks(void) {
    XrXirValue sentinel={XR_XIR_I64,0,123},saved=sentinel;XrXirCallView invalid={.state=&sentinel};
    XrXirAction refused=xr_xir_instance_native_callable(&invalid);
    CHECK(refused.kind==XR_XIR_ACTION_FAULT && refused.value.type==XR_XIR_I64 && refused.value.payload==XR_XIR_CALL_BAD_STATE && !memcmp(&sentinel,&saved,sizeof(saved)));
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"from-utf8-active-guards",NULL};
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirSourceProductRequest request={{session,NULL,&authority,&context,XR_PUBLIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL},target};
    const char text[]="export fn probe()->i64 { return 42 }\n";XrXirSourceText input={NULL,text,sizeof(text)-1};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic failure={0};XrFrontendRoot failure_root={.message=""};
    CHECK(xr_xir_compile_source_product_build_text(&request,&input,&product,&failure)==XR_XIR_OK);
    failure_root=(XrFrontendRoot){.message=""};
    if(failure.failure.report) {
        XrFrontendReportInfo info={0};CHECK(xr_frontend_report_info(failure.failure.report,&info)&&info.roots==1);
        CHECK(xr_frontend_report_root(failure.failure.report,0,&failure_root));
    }
    xr_compile_session_free(session);XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    XrXirArtifact *closed=NULL,*code=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&failure);
    CHECK(xr_xir_compile_lower(closed,&target,&code,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(code);CHECK(module->function_count<=8);uint32_t probe=UINT32_MAX;
    XrXirCallEntry entries[8];XrXirVmBinding bindings[8];
    for (uint32_t f=0;f<module->function_count;++f) {
        CHECK(xr_xir_compile_vm_bind(code,f,&bindings[f],&entries[f])==XR_XIR_OK);
        CHECK(!module->declarations->functions[f].native_callable);
        if (module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"probe",5)) probe=f;
    }
    CHECK(probe!=UINT32_MAX);XrXirCallEntry original=entries[probe];
    for (uint32_t bytes=0;bytes<=sizeof(guard_marker);bytes+=(uint32_t)sizeof(guard_marker)) {
        GuardProbe state={.bytes=bytes};entries[probe]=original;entries[probe].state_bytes=bytes;
        entries[probe].resume=guard_probe;entries[probe].release=guard_release;entries[probe].environment=&state;
        XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,module->declarations,{0},module->types,{0}};
        spec.packet=xir_program_packet_borrow(code);XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
        CHECK(xr_xir_instance_start(instance,probe,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(state.calls==1 && state.releases==1);
    }
    xr_xir_compile_artifact_free(code);CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);puts("Strict fromUtf8 real active ordinary ID0 zero/non-result state rejection PASS");
}
static void result_lifetime(void) {
    const char source[]="export fn owned() throws(Utf8Error)->string {var bytes:Array<u8>=[65,0,195,169];const text=try string.fromUtf8(bytes[:]);bytes[0]=66;return text}\n";
    XrSourceArtifact *artifact=build_text(XR_SOURCE_TARGET_VM,source,sizeof(source)-1);
    XrSourceExecutionOptions options=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_OK);XrSourceCallOptions call_options;
    CHECK(xray_source_call_options_init(&call_options,sizeof(call_options))==XR_SOURCE_OK);
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="owned";invocation.name_length=5;XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&call_options,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);XrSourceEvent event={0};
    for(unsigned i=0;i<4096;++i) {CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;}
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);XrSourceResult *result=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    const char *bytes=NULL;size_t length=0;CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK && length==4 && !memcmp(bytes,"A\0\xc3\xa9",4));
    CHECK(compile_cancel.polls==compile_cancel.finished);xray_source_result_free(result);
    puts("Strict fromUtf8 named owned String survives input/request/artifact/execution/call retirement PASS");
}
#include "try_assert_source_cases.h"
static int source_case(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--try-rejections")) {try_assert_source_rejections();return 0;}
    if (argc==2 && !strcmp(argv[1],"--try-graph")) {try_assert_source_graph();return 0;}
    if (argc==2 && !strcmp(argv[1],"--result-life")) {result_lifetime();return 0;}
    if (argc==2 && !strcmp(argv[1],"--guards")) {guard_checks();return 0;}
    if (argc==2 && !strcmp(argv[1],"--library")) {library_check(NULL);return 0;}
    if (argc==3 && !strcmp(argv[1],"--emit-library")) {library_check(argv[2]);return 0;}
    if (argc==2 && !strcmp(argv[1],"--source-rejections")) {source_rejections();return 0;}
    if (argc==2 && !strcmp(argv[1],"--held")) {held_checks();return 0;}
    if (argc==3 && !strcmp(argv[1],"--historical")) {historical_check(argv[2]);return 0;}
    if (argc==3 && (!strcmp(argv[1],"--emit") || !strcmp(argv[1],"--emit-local"))) {
        bool local=!strcmp(argv[1],"--emit-local");
        XrSourceArtifact *artifact=build_text(XR_SOURCE_TARGET_C,local?from_utf8_local_source:from_utf8_source,
            local?sizeof(from_utf8_local_source)-1:sizeof(from_utf8_source)-1);const char *text=NULL;size_t length=0;
        CHECK(xray_source_artifact_c_text(artifact,&text,&length)==XR_SOURCE_OK && length);
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(text,1,length,file)==length && !fclose(file));
        xray_source_artifact_free(artifact);return 0;
    }
    bool identities=argc==2 && !strcmp(argv[1],"--identity");
    bool physical=false;size_t physical_index=0;
#if defined(XR_FROM_UTF8_WITH_OBSERVER)
    physical=argc==2 && (!strcmp(argv[1],"--physical-header") || !strcmp(argv[1],"--physical-bytes") || !strcmp(argv[1],"--cancel-after-allocation"));
    if (physical) {physical_index=!strcmp(argv[1],"--cancel-after-allocation")?2u:(size_t)!strcmp(argv[1],"--physical-bytes");xr_test_from_utf8_fault_begin(physical_index);}
#endif
    bool local=physical || identities || (argc==2 && !strcmp(argv[1],"--local"));
    bool owned_case=argc==2 && !strcmp(argv[1],"--owned");
    bool invalid_case=argc==2 && !strcmp(argv[1],"--invalid");
    const char *text=local?from_utf8_local_source:from_utf8_source;
    size_t length=local?sizeof(from_utf8_local_source)-1:sizeof(from_utf8_source)-1;char *owned=NULL;
    const char *expected=from_utf8_expected;size_t expected_length=sizeof(from_utf8_expected)-1;
    if (owned_case) {text=from_utf8_owned_source;length=sizeof(from_utf8_owned_source)-1;expected=from_utf8_owned_expected;expected_length=sizeof(from_utf8_owned_expected)-1;}
    if (invalid_case) {text=from_utf8_invalid_source;length=sizeof(from_utf8_invalid_source)-1;expected=from_utf8_invalid_expected;expected_length=sizeof(from_utf8_invalid_expected)-1;}
    bool product=argc==3 && !strcmp(argv[1],"--product-file");
    bool emit_file=argc==4 && !strcmp(argv[1],"--emit-file");
    bool bad_state=argc==4 && !strcmp(argv[1],"--vm-file") && !strcmp(argv[3],"--expect-bad-state");
    bool external=(argc==3 || argc==5 || bad_state) && (!strcmp(argv[1],"--vm-file") || product);
    CHECK(argc==1 || local || owned_case || invalid_case || external || emit_file);
    if (argc==5) CHECK(!strcmp(argv[3],"--expect"));
    if (external || emit_file) {
        FILE *file=fopen(argv[2],"rb");CHECK(file && !fseek(file,0,SEEK_END));long count=ftell(file);
        CHECK(count>=0 && !fseek(file,0,SEEK_SET));length=(size_t)count;owned=malloc(length+1);CHECK(owned);
        CHECK(fread(owned,1,length,file)==length && !fclose(file));owned[length]=0;text=owned;
    }
    if (emit_file) {
        XrSourceArtifact *artifact=build_text(XR_SOURCE_TARGET_C,text,length);free(owned);
        const char *code=NULL;size_t bytes=0;CHECK(xray_source_artifact_c_text(artifact,&code,&bytes)==XR_SOURCE_OK && bytes);
        FILE *file=fopen(argv[3],"wb");CHECK(file && fwrite(code,1,bytes,file)==bytes && !fclose(file));
        xray_source_artifact_free(artifact);return 0;
    }
    if (product || identities) {int result=product_check(text,length,identities,NULL,NULL,NULL,NULL);free(owned);return result;}
    XrSourceArtifact *artifact=build_text(XR_SOURCE_TARGET_VM,text,length);free(owned);
    XrSourceExecutionOptions options=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_OK);
    XrSourceCallOptions call_options;CHECK(xray_source_call_options_init(&call_options,sizeof(call_options))==XR_SOURCE_OK);
    Output output={0};call_options.context=&output;call_options.write=write_bytes;XrSourceCall *call=NULL;
    CHECK(xray_source_artifact_begin(execution,artifact,&call_options,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    XrSourceEvent event={0};for (unsigned i=0;i<4096;++i) {
        CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;
    }
    XrSourceRunState terminal=physical?(physical_index==2?XR_SOURCE_RUN_CANCELLED:XR_SOURCE_RUN_OUT_OF_MEMORY):bad_state?XR_SOURCE_RUN_API_ERROR:XR_SOURCE_RUN_RETURNED;
    CHECK(event.state==terminal);
    if (bad_state) CHECK(event.engine_status==XR_XIR_CALL_BAD_STATE && !output.length);
    if (physical) CHECK(event.engine_status==(uint32_t)(physical_index==2?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_OOM) && !output.length);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    XrSourceEvent cleanup={0};CHECK(xray_source_call_drop(&call,&cleanup)==XR_SOURCE_OK && !call);
    if (bad_state) CHECK(cleanup.state==XR_SOURCE_RUN_CLEANUP_FAILED && cleanup.engine_status==XR_XIR_CALL_BAD_STATE);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==terminal);
    xray_source_result_free(result);CHECK(compile_cancel.polls==compile_cancel.finished);
#if defined(XR_FROM_UTF8_WITH_OBSERVER)
    if (physical) {xr_test_from_utf8_fault_finish(physical_index);return 0;}
#else
    (void)physical_index;
#endif
    if (external) {
        if (argc==5) {
            FILE *file=fopen(argv[4],"rb");CHECK(file && !fseek(file,0,SEEK_END));long bytes=ftell(file);
            CHECK(bytes>=0 && (size_t)bytes==output.length && !fseek(file,0,SEEK_SET));
            char oracle[sizeof(output.bytes)];CHECK(fread(oracle,1,output.length,file)==output.length && !fclose(file));
            CHECK(!memcmp(output.bytes,oracle,output.length));
        }
        CHECK(fwrite(output.bytes,1,output.length,stdout)==output.length);return 0;
    }
    CHECK(output.length==expected_length && !memcmp(output.bytes,expected,output.length));
    puts("Strict fromUtf8 Source/VM: owned Unicode/NUL and typed catch PASS");return 0;
}

int main(int argc,char **argv) {
    if (argc!=1) return source_case(argc,argv);
    CHECK(!source_case(argc,argv));
#if !defined(XR_FROM_UTF8_WITH_OBSERVER)
    const char *modes[]={"--local","--owned","--invalid","--identity","--source-rejections","--held","--library","--guards","--result-life"};
    for (size_t i=0;i<sizeof(modes)/sizeof(*modes);++i) {
        char *arguments[]={argv[0],(char *)modes[i]};CHECK(!source_case(2,arguments));
    }
    char *historical[]={argv[0],"--historical",XR_FROM_UTF8_HISTORICAL};CHECK(!source_case(3,historical));
#endif
    return 0;
}
#endif
