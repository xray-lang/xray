/* Real Built -> Checked -> specialization -> Lowered -> generated C producer. */
#include "../xir_construction_fixture.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
int main(int argc,char **argv) {
    CHECK(argc==3);const XrCompileResourceLimits limits={16777216,8388608,67108864};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirInstruction init={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    XrXirInstruction code[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}};
    const XrXirBlock blocks[]={{0,1,0,0},{0,2,0,0}};
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,code,2,NULL,0}};
    const XrXirSourceModule source={"root",4,NULL,0,0};
    const XrXirFunctionIdentity functions_id[]={{0},{0}};
    const XrXirDeclarations declarations={&source,1,functions_id,NULL,0,NULL,0,0,1,NULL};
    const XrXirNominalDeclaration nominals[]={
        {.module={"root",4},.name={"First",5},.exported=1,.kind=XR_XIR_NOMINAL_STRUCT},
        {.module={"root",4},.name={"Second",6},.exported=1,.kind=XR_XIR_NOMINAL_STRUCT}};
    const XrXirNominalTable table={nominals,2,NULL};
    const XrXirTypes types={NULL,0,&table,NULL};
    const XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(&context, &module, &checked, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *lowered_module=xr_xir_compile_artifact_module(lowered);
    CHECK(lowered_module->types && lowered_module->types->nominals && lowered_module->types->nominals->count==2);
    CHECK(lowered_module->types->nominals->identities && !lowered_module->types->nominals->declarations);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,argv[2],1048576,&c)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    xr_compile_resources_release(context.resources);
    CHECK(c.text && c.length && !strstr(c.text,"({"));FILE *file=fopen(argv[1],"wb");CHECK(file);
    CHECK(fwrite(c.text,1,c.length,file)==c.length && !fclose(file));xr_xir_compile_c_source_free(&c);
    CHECK(!c.text && !c.length);return 0;
}
