/* Current Rune predicate admission, independent scalar oracle and VM execution. */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"whitespace %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_construction_fixture.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "base/xsha256.h"
#include "rune_whitespace_cases.h"
static void previous110_case(const char *path) {
    unsigned char bytes[31995],digest[32];char hexadecimal[65];
    FILE *file=fopen(path,"rb");CHECK(file && fread(bytes,1,sizeof(bytes),file)==sizeof(bytes));
    CHECK(fgetc(file)==EOF && !ferror(file) && !fclose(file));
    CHECK(bytes[8]==39 && !bytes[9] && !bytes[10] && !bytes[11] && bytes[12]==110 &&
        !bytes[13] && !bytes[14] && !bytes[15]);
    xr_sha256(bytes,sizeof(bytes),digest);
    for(unsigned i=0;i<32;++i)CHECK(snprintf(hexadecimal+2*i,3,"%02x",digest[i])==2);
    CHECK(!strcmp(hexadecimal,"f1f13d7a158c6823ddd282a201bda699064da196bc5dda12b6a6c22b67ff6367"));
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_checked_read(&context,bytes,sizeof(bytes),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_checked_read(&context,bytes,64,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(resources);
}
static XrXirArtifact *whitespace_build(const XrXirCompileContext *context) {
    XrXirType parameter=XR_XIR_RUNE;
    XrXirInstruction code[]={
        {.op=XR_XIR_RUNE_IS_WHITESPACE,.type=XR_XIR_BOOL,.args={0}},
        {.op=XR_XIR_RETURN,.args={1}}};
    XrXirBlock block={0,2,0,0};
    XrXirFunction function={.name="whitespace",.name_length=10,.parameters=&parameter,.parameter_count=1,
        .result=XR_XIR_BOOL,.blocks=&block,.block_count=1,.instructions=code,.instruction_count=2,.error_type=XR_XIR_NEVER};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(context,&module,&checked,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL;
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    for(unsigned attack=0;attack<5;++attack) {
        XrXirInstruction saved=code[0];XrXirType old=parameter;
        if(attack==0)code[0].type=XR_XIR_I64;
        if(attack==1)parameter=XR_XIR_I64;
        if(attack==2)code[0].args[1]=1;
        if(attack==3)code[0].immediate=1;
        if(attack==4)code[0].type_arguments[1]=1;
        XrXirArtifact *rejected=NULL;
        XrXirStatus status=xir_fixture_check(context,&module,&rejected,NULL);
        CHECK(status==(attack<2?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE) && !rejected);
        code[0]=saved;parameter=old;
    }
    return lowered;
}
static XrXirRunStatus whitespace_vm(void *owner,XrXirRunContext *run,const XrXirValue *input,XrXirValue *output) {
    return xr_xir_compile_vm_run(owner,0,run,input,1,output);
}
int main(int argc,char **argv) {
    _Static_assert(XR_XIR_CONST_RUNE==134 && XR_XIR_INDEX_CHECK==207 &&
        XR_XIR_RUNE_IS_WHITESPACE==XR_XIR_INDEX_CHECK+1,
        "the predicate appends without changing existing operation identities");
    if(argc==3 && !strcmp(argv[1],"--previous110")){previous110_case(argv[2]);return 0;}
    CHECK(argc==1 || argc==2);
    uint32_t count=0;
    for(uint32_t code=0;code<=0x10ffff;++code) if(code<0xd800 || code>0xdfff) {
        int64_t value=-1;CHECK(xr_xir_rune_is_whitespace(code,&value)==XR_XIR_RUN_OK);
        CHECK(value==whitespace_expected(code));count+=(uint32_t)value;
    }
    CHECK(count==24);
    const int64_t invalid[]={-1,0xd800,0xdfff,0x110000,INT64_MAX};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        int64_t untouched=99;CHECK(xr_xir_rune_is_whitespace(invalid[i],&untouched)==XR_XIR_RUN_BAD_ARGUMENT && untouched==99);
    }
    CHECK(xr_xir_rune_is_whitespace(32,NULL)==XR_XIR_RUN_BAD_ARGUMENT);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirArtifact *artifact=whitespace_build(&context);whitespace_cases(whitespace_vm,artifact);
    XrXirCSource source={0};CHECK(xr_xir_compile_emit_leaf_c(artifact,"rune_whitespace",1048576,&source)==XR_XIR_OK);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
    }
    xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(artifact);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(resources);return 0;
}
