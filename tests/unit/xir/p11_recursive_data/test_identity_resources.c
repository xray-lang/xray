#include "recursive_common.h"
#include "recursive_resources.h"
#include "recursive_limits.h"
static void identity_control(const char *name) {
    RecursiveInput input;recursive_load(&input,name,true,true);
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(recursive_source(&context,&input,&source,&diagnostic)==XR_XIR_OK && source.checked && source.snapshot);
    xr_xir_compile_source_result_free(&source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    recursive_load(&input,name,false,true);context_new(&context);
    XrXirStatus status=recursive_source(&context,&input,&source,&diagnostic);
    fprintf(stderr,"Identity negative %s ordinary checked, folded status=%u %d:%d %s\n",name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_BAD_TYPE && !source.checked && !source.snapshot);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc>=2);
    if (!strcmp(argv[1],"controls")) {
        CHECK(argc==2);identity_control("canonical-self-cycle.xr");identity_control("captured-cell-cycle.xr");return 0;
    }
    CHECK(argc==4 || argc==5);RecursiveInput input;bool evaluator=!strcmp(argv[1],"evaluator") || !strcmp(argv[1],"limits");
    recursive_load(&input,argv[2],evaluator,false);
    if (!strcmp(argv[1],"source")) {
        XrCompileResourceStats need={0};size_t sites=recursive_source_census(&input,&need);
        if (!strcmp(argv[3],"fi") || !strcmp(argv[3],"ends")) recursive_source_faults(&input,sites,!strcmp(argv[3],"ends"),&need);
        else { CHECK(!strcmp(argv[3],"bounds"));recursive_source_bounds(&input,&need); }
    } else if (!strcmp(argv[1],"packet")) {
        CHECK(!strcmp(argv[3],"fi") || !strcmp(argv[3],"ends"));recursive_packet_faults(&input,!strcmp(argv[3],"fi"),!strcmp(argv[3],"ends"));
    } else {
        RecursiveWire wire;recursive_wire_new(&wire,&input);
        if (!strcmp(argv[1],"limits")) { recursive_limits(&wire,"eval",false);recursive_original_owner(&wire,"eval",false);recursive_last_owner(&wire,"eval",false);return 0; }
        CHECK(!strcmp(argv[1],"evaluator"));XrCompileResourceStats need={0};size_t sites=recursive_evaluator_census(&wire,"eval",false,&need);
        CHECK(!strcmp(argv[3],"fi") || !strcmp(argv[3],"ends"));recursive_evaluator_faults(&wire,"eval",false,sites,!strcmp(argv[3],"ends"),&need);
        recursive_wire_free(&wire);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
}
