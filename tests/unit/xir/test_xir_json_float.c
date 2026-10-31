/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_float.c - Cold ordinary JSON finite floating receiver
 */
#include "base/xsha256.h"
#include "base/xresource_limits.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_float_runtime.h"
static XrXirCompileContext json_float_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
typedef struct JsonFloatSource {const char *text,*logical;XrXirLinkageKind kind;const XrXirLibraryCatalog *catalog;} JsonFloatSource;
static XrXirStatus json_float_source_check(const XrXirCompileContext *context,const JsonFloatSource *source,
    XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_FLOAT_WORK};char path[1024];
    CHECK(snprintf(path,sizeof(path),"%s/%s",XR_JSON_FLOAT_WORK,source->logical)>0);
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,source->kind,source->catalog};
    XrXirSourceText input={source->logical,source->text,strlen(source->text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static XrXirSourceResult json_float_source(const XrXirCompileContext *context,const JsonFloatSource *source) {
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=json_float_source_check(context,source,&result,&diagnostic);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"JSON floating %s Source%u %d:%d %s spent=%llu/%llu/%llu\n",source->logical,status,diagnostic.line,diagnostic.column,
        diagnostic.message,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.allocation_count);
    if(status!=XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);xr_xir_compile_source_result_free(&result);
        /* The strict positive fixture aborts after discarding its caller setup. */
        xr_xir_compile_library_catalog_free((XrXirLibraryCatalog *)source->catalog);xr_compile_resources_release(context->resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    }
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);return result;
}
static void json_float_read(const char *path,char buffer[24000]) {
    FILE *file=fopen(path,"rb");CHECK(file);size_t size=fread(buffer,1,23999,file);CHECK(!ferror(file) && feof(file) && !fclose(file));buffer[size]=0;
}
static XrXirLibraryCatalog *json_float_catalog(const XrXirCompileContext *context,XrXirSourceResult *source,unsigned count) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);CHECK(module->declarations->module_count==count);
    XrXirLibraryModuleInput bindings[2]={{0}};const char *paths[]={"number-decoding.xr","float-decoding.xr"};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_FLOAT_WORK};
    for(unsigned path=0;path<count;++path) {
        char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(context->resources,&authority,paths[path],&canonical)==XR_MODULE_OK);
        unsigned matched=0;
        for(unsigned i=0;i<count;++i)if(module->declarations->modules[i].name_length==strlen(canonical) &&
            !memcmp(module->declarations->modules[i].name,canonical,strlen(canonical))){CHECK(!bindings[i].logical_path);bindings[i]=(XrXirLibraryModuleInput){authority,paths[path]};++matched;}
        xr_compile_resources_free(canonical);CHECK(matched==1);
    }
    XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,count};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"JSON floating Catalog modules%u status%u spent=%llu/%llu/%llu\n",count,status,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.allocation_count);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);memset(bindings,0xa5,sizeof(bindings));
    memset(&input,0xa5,sizeof(input));xr_xir_compile_source_result_free(source);
    if(status!=XR_XIR_OK) {
        CHECK(!catalog);xr_compile_resources_release(context->resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
        fprintf(stderr,"JSON floating Catalog refusal released every compiler/runtime owner before strict positive assertion\n");
    }
    CHECK(status==XR_XIR_OK);return catalog;
}
static uint32_t json_float_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static void json_float_source_refusal(const XrXirCompileContext *context,const JsonFloatSource *source,
    const char *message) {
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(json_float_source_check(context,source,&result,&diagnostic)!=XR_XIR_OK);
    CHECK(!result.checked && !result.snapshot && strstr(diagnostic.message,message));
    xr_xir_compile_source_result_free(&result);
}
static void json_float_packet_digest(XrXirCheckedPacket *packet) {
    CHECK(packet->length>=64);XrSHA256Context sha;
    xr_sha256_init(&sha);xr_sha256_update(&sha,packet->bytes,32);
    xr_sha256_update(&sha,packet->bytes+64,packet->length-64);xr_sha256_final(&sha,packet->bytes+32);
}
/* Corrupt complete wire facts while keeping both packet digests authentic. */
static void json_float_state_fact_refusals(const XrXirCompileContext *context,XrXirCheckedPacket *packet,
    XrXirLibraryInput *input) {
    static const uint8_t fact[]={1,0,0,0,0,0,0,0,6,0,0,0,'s','t','o','r','e','d',0,0,0,0};
    size_t position=0;unsigned matches=0;
    for(size_t at=64;at+sizeof(fact)<=packet->length;++at)
        if(!memcmp(packet->bytes+at,fact,sizeof(fact))){position=at;++matches;}
    CHECK(matches==1);
    const unsigned offsets[]={0,4,18};
    for(unsigned i=0;i<3;++i) {
        size_t at=position+offsets[i];uint8_t saved=packet->bytes[at];packet->bytes[at]=i==2?2:i?1:0;
        json_float_packet_digest(packet);xr_sha256(packet->bytes,packet->length,input->sha256);
        XrXirLibraryCatalog *catalog=NULL;
        XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,input,1,&catalog);
        CHECK(!catalog && (status==XR_XIR_BAD_STRUCTURE || (i==2 && status==XR_XIR_BAD_STAGE)));
        packet->bytes[at]=saved;
    }
    json_float_packet_digest(packet);xr_sha256(packet->bytes,packet->length,input->sha256);
}
/* Private immutable binary64 state uses the ordinary receiving slot path. */
static void json_float_catalog_state_boundaries(void) {
    static const struct {const char *text;XrXirType type;bool mutable,exported;XrXirStatus expected;} cases[]={
        {"const stored:i64=1\nexport fn observe()->i64{return stored}\n",XR_XIR_I64,false,false,XR_XIR_OK},
        {"const stored:f64=1.0\nexport fn observe()->f64{return stored}\n",XR_XIR_F64,false,false,XR_XIR_OK},
        {"const stored:f32=1.0\nexport fn observe()->f32{return stored}\n",XR_XIR_F32,false,false,XR_XIR_OK},
        {"export const stored:f64=1.0\nexport fn observe()->f64{return stored}\n",XR_XIR_F64,false,true,XR_XIR_OK},
        {"export const stored:f32=1.0\n",XR_XIR_F32,false,true,XR_XIR_OK},
        {"var stored:f64=1.0\nexport fn observe()->f64{return stored}\n",XR_XIR_F64,true,false,XR_XIR_BAD_STAGE},
        {"var stored:f32=1.0\nexport fn observe()->f32{return stored}\n",XR_XIR_F32,true,false,XR_XIR_BAD_STAGE}
    };
    XrXirCompileContext context=json_float_context();
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        JsonFloatSource request={cases[i].text,"number-decoding.xr",XR_XIR_LIBRARY,NULL};
        XrXirSourceResult result=json_float_source(&context,&request);
        const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
        CHECK(module->declarations->module_count==1 && module->declarations->slot_count==1);
        const XrXirSlot *slot=&module->declarations->slots[0];
        XrXirType logical=xr_xir_type_is_cell(module->types,slot->type) ? xr_xir_cell_element(module->types,slot->type) : slot->type;
        CHECK(logical==cases[i].type && !!slot->mutable==cases[i].mutable);
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&result);
        XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_FLOAT_WORK},"number-decoding.xr"};
        XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
        bool floating=cases[i].type==XR_XIR_F32 || cases[i].type==XR_XIR_F64;
        if(floating && !cases[i].mutable && !cases[i].exported) json_float_state_fact_refusals(&context,&packet,&input);
        XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog);
        CHECK((status==XR_XIR_OK)==(catalog!=NULL));
        const char *text=cases[i].type==XR_XIR_F32 ?
            "import {stored} from \"./number-decoding\"\nexport fn read()->f32{return stored}\n" :
            "import {stored} from \"./number-decoding\"\nexport fn read()->f64{return stored}\n";
        if(catalog && floating && !cases[i].exported) {
            JsonFloatSource private_read={text,
                "private-reader.xr",XR_XIR_PROGRAM,catalog};
            json_float_source_refusal(&context,&private_read,"export");
        }
        if(catalog && cases[i].exported) {
            JsonFloatSource public_read={text,
                "public-reader.xr",XR_XIR_PROGRAM,catalog};
            XrXirSourceResult received=json_float_source(&context,&public_read);
            xr_xir_compile_source_result_free(&received);
        }
        xr_xir_compile_library_catalog_free(catalog);
        memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
        XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.held_bytes==baseline.held_bytes && !runtime_live && !runtime_bytes);
        fprintf(stderr,"JSON floating Catalog state case%zu type%u mutable%u status%u expected%u allocations%llu; held baseline restored\n",
            i,(unsigned)logical,(unsigned)cases[i].mutable,status,cases[i].expected,(unsigned long long)after.allocation_count);
        CHECK(status==cases[i].expected);
    }
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--catalog-state")){json_float_catalog_state_boundaries();return 0;}
    CHECK(argc==1 || argc==3);XrXirCompileContext context=json_float_context();char text[24000];json_float_read(XR_JSON_NUMBER_SOURCE,text);
    JsonFloatSource request={text,"number-decoding.xr",XR_XIR_LIBRARY,NULL};XrXirSourceResult result=json_float_source(&context,&request);
    memset(text,0xa5,sizeof(text));XrXirLibraryCatalog *numbers=json_float_catalog(&context,&result,1);
    json_float_read(XR_JSON_FLOAT_SOURCE,text);
    static const char floating_proof[]=
        "\nexport const stored32:f32=1.5\nexport fn float32Value(x:f32)->f32{return stored32+x}\n"
        "const storedFinite:f64=1.5\nconst storedZero:f64=-0.0\n"
        "const storedInfinity:f64=1.0/0.0\nconst storedNan:f64=0.0/0.0\n"
        "export fn floatState()->bool {return storedFinite==1.5 && 1.0/storedZero<0.0 && "
        "storedInfinity>1.7976931348623157e308 && storedNan!=storedNan}\n"
        "\nstruct FloatProbe {value:f64 init(value:f64){this.value=value}}\n"
        "fn floatIdentity<T>(value:T)->T{return value}\n"
        "export fn floatArithmetic(x:f64)->bool {const box=FloatProbe(x);const value=floatIdentity<f64>(box.value);"
        "const zero=value-value;const nan=zero/zero;"
        "return (value+1.0==3.0 && value-1.0==1.0 && value*2.0==4.0 && value/2.0==1.0 && -value == -2.0 && "
        "value==2.0 && value!=3.0 && value<3.0 && value<=2.0 && value>1.0 && value>=2.0 && "
        "nan!=nan && !(nan==nan) && !(nan<value) && !(nan<=value) && !(nan>value) && !(nan>=value))}\n";
    size_t used=strlen(text);CHECK(sizeof(floating_proof)<=sizeof(text)-used);memcpy(text+used,floating_proof,sizeof(floating_proof));
    request=(JsonFloatSource){text,"float-decoding.xr",XR_XIR_LIBRARY,numbers};result=json_float_source(&context,&request);
    memset(text,0xa5,sizeof(text));xr_xir_compile_library_catalog_free(numbers);XrXirLibraryCatalog *library=json_float_catalog(&context,&result,2);
    static const char consumer[]=
        "import {jsonFloat,floatArithmetic,floatState,stored32,float32Value} from \"./float-decoding\"\n"
        "export fn inspect(text:string)->f64? {return jsonFloat(text)}\n"
        "export fn main()->i64 {const value=comptime jsonFloat(\"1.7976931348623157e308\");const arithmetic=comptime floatArithmetic(2.0);"
        "return (arithmetic && floatArithmetic(2.0) && floatState() && stored32==1.5 && float32Value(0.5)==2.0 && value!=null && value! == 1.7976931348623157e308)?42:0}\n"
        "export fn observed()->f64? {return jsonFloat(\"1.7976931348623157e308\")}\n";
    request=(JsonFloatSource){consumer,"receiver.xr",XR_XIR_PROGRAM,library};result=json_float_source(&context,&request);xr_xir_compile_library_catalog_free(library);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    if(argc==3){FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);const uint32_t selection[3]={json_float_entry(module,"main"),json_float_entry(module,"inspect"),json_float_entry(module,"observed")};
    if(argc==3) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"json_float",8388608,&source)==XR_XIR_OK);FILE *file=fopen(argv[1],"wb");CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length);CHECK(fprintf(file,"\nconst uint32_t json_float_selection[3]={%uu,%uu,%uu};\nconst uint32_t json_float_callee=%uu;\n",selection[0],selection[1],selection[2],json_float_entry(module,"jsonFloat"))>0 && !fclose(file));xr_xir_compile_c_source_free(&source);
    }
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"JSON floating Program status%u before=%llu/%llu/%llu after=%llu/%llu/%llu\n",status,
        (unsigned long long)before.allocated_bytes,(unsigned long long)before.peak_bytes,(unsigned long long)before.allocation_count,
        (unsigned long long)after.allocated_bytes,(unsigned long long)after.peak_bytes,(unsigned long long)after.allocation_count);
    if(status!=XR_XIR_OK) {
        CHECK(!program && lowered);xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
        fprintf(stderr,"JSON floating Program failure released all compiler/runtime owners before strict positive assertion\n");
    }
    CHECK(status==XR_XIR_OK && !lowered);
    XrXirValue kept=json_float_cases(program,selection);xr_compile_resources_release(context.resources);json_float_retained(&kept);return 0;
}
