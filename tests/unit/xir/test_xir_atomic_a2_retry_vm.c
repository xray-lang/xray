/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_a2_retry_vm.c - Bounded backend retry and physical ownership tests
 *
 * KEY CONCEPT:
 *   Real production bodies retain captured operands across exactly one CAS per poll.
 */
#define main atomic_backend_source_producer_main
#include "test_xir_atomic_a2_source.c"
#undef main
#include "xir_atomic_a2_backend_cases.h"
int main(int argc,char **argv) {
    if(argc==2)return atomic_backend_source_producer_main(argc,argv);
    CHECK(argc==1);LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    AtomicPipeline capture={0};CHECK(atomic_source_pipeline(&owner.context,&capture)==XR_XIR_OK && capture.program);
    atomic_backend_cases(capture.program,capture.entry);xr_xir_compile_program_drop(capture.program);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts("VM forced f64 retry: same poll frontier, one operand evaluation, consecutive RMW, cancellation and physical release passed");
    return 0;
}
