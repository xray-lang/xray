#include "recursive_common.h"
#include "recursive_controls.h"
#include "recursive_resources.h"

int main(int argc,char **argv) {
    CHECK(argc>=2);
    if (!strcmp(argv[1],"controls")) {
        CHECK(argc==2);recursive_metadata_controls();
        recursive_family_controls("recursive-class.xr",false);recursive_family_controls("recursive-class-export.xr",true);
        recursive_family_controls("recursive-function.xr",false);recursive_family_controls("recursive-function-export.xr",true);return 0;
    }
    CHECK(argc==4 || argc==5);bool full=!strcmp(argv[3],"fi"),ends=!strcmp(argv[3],"ends"),bounds=!strcmp(argv[3],"bounds");
    CHECK(full || ends || bounds || !strcmp(argv[3],"census"));RecursiveInput input;
    bool evaluator=!strcmp(argv[1],"evaluator");recursive_load(&input,argv[2],evaluator,false);
    if (!strcmp(argv[1],"source")) {
        CHECK(argc==4);XrCompileResourceStats used={0};size_t sites=recursive_source_census(&input,&used);
        if (full || ends) recursive_source_faults(&input,sites,ends,&used);
        if (bounds) recursive_source_bounds(&input,&used);
    } else if (!strcmp(argv[1],"packet")) { CHECK(argc==4 && !bounds);recursive_packet_faults(&input,full,ends); }
    else {
        CHECK(evaluator && argc==5 && !bounds);bool owning=!strcmp(argv[4],"make");RecursiveWire wire;recursive_wire_new(&wire,&input);
        XrCompileResourceStats need={0};size_t sites=recursive_evaluator_census(&wire,argv[4],owning,&need);
        if (full || ends) recursive_evaluator_faults(&wire,argv[4],owning,sites,ends,&need);
        recursive_wire_free(&wire);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
}
