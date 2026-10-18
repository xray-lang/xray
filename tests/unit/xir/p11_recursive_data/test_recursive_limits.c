#include "recursive_common.h"
#include "recursive_limits.h"

int main(int argc,char **argv) {
    CHECK(argc==3);bool owning=!strcmp(argv[2],"make");RecursiveInput input;recursive_load(&input,argv[1],true,false);
    RecursiveWire wire;recursive_wire_new(&wire,&input);
    recursive_limits(&wire,argv[2],owning);recursive_original_owner(&wire,argv[2],owning);
    XrXirCompileContext context;context_new(&context);XrXirArtifact *lowered=NULL;
    CHECK(recursive_pipeline(&context,&wire.packet,&lowered)==XR_XIR_OK);xr_compile_resources_release(context.resources);
    recursive_execute(&lowered);
    recursive_last_owner(&wire,argv[2],owning);return 0;
}
