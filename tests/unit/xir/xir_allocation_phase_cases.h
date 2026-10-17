/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 *
 * Full original compiler ordinals and independently owned runtime receivers.
 * Witnesses outlive the one actual Program and all per-attempt escaped gates.
 */
#ifndef XIR_ALLOCATION_PHASE_CASES_H
#define XIR_ALLOCATION_PHASE_CASES_H
#include "xir_allocation_phase_map.h"

static void function_allocation_phase_case(bool cancel) {
    allocation_phase_case_begin("Function",0,cancel);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(function_case_run(cancel) && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;unsigned releases=0;XrXirProgram *program=NULL;
    allocation_request_begin(&allocation_prefix_requests);
    allocation_calls=0;CHECK(function_case_seal(&releases,&program)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("Function",0,cancel,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!function_case_run(cancel));allocation_phase_compile_end(&map,ordinal);
    }
    releases=0;allocation_calls=0;
    CHECK(function_case_seal(&releases,&program)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(function_case_execute(program,&releases,cancel) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!function_case_run(cancel));allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!function_case_execute(program,&releases,cancel));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(releases==1);allocation_phase_complete(&map);
}

static void function_producer_allocation_phase_case(void) {
    allocation_phase_case_begin("Producer",0,0);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(function_producer_owner_run() && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;ProducerRuntimeWitness witness={0};
    XrXirProgram *program=NULL;uint32_t make=0,nested=0;
    allocation_request_begin(&allocation_prefix_requests);
    allocation_calls=0;CHECK(producer_runtime_seal(&witness,&program,&make,&nested)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("Producer",0,0,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!function_producer_owner_run());allocation_phase_compile_end(&map,ordinal);
    }
    witness=(ProducerRuntimeWitness){0};allocation_calls=0;
    CHECK(producer_runtime_seal(&witness,&program,&make,&nested)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(function_producer_execute(program,&witness,make,nested) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!witness.releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!function_producer_owner_run());allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!function_producer_execute(program,&witness,make,nested));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!witness.releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1);allocation_phase_complete(&map);
}

static void invocation_runtime_allocation_phase_case(unsigned mode) {
    allocation_phase_case_begin("runtime",0,mode);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(invocation_runtime_owner_run(mode,NULL,NULL) && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;EdgeRuntimeWitness witness={0};witness.mode=mode;
    XrXirProgram *program=NULL;allocation_request_begin(&allocation_prefix_requests);allocation_calls=0;
    CHECK(edge_runtime_seal(&witness,&program)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("runtime",0,mode,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!invocation_runtime_owner_run(mode,NULL,NULL));allocation_phase_compile_end(&map,ordinal);
    }
    witness=(EdgeRuntimeWitness){0};witness.mode=mode;allocation_calls=0;
    CHECK(edge_runtime_seal(&witness,&program)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(invocation_runtime_execute(program,&witness,NULL,NULL) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!witness.releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!invocation_runtime_owner_run(mode,NULL,NULL));allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!invocation_runtime_execute(program,&witness,NULL,NULL));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!witness.releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1);allocation_phase_complete(&map);
}

static void invocation_direct_allocation_phase_case(unsigned mode) {
    allocation_phase_case_begin("direct",0,mode);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(invocation_direct_owner_run(mode,NULL,NULL) && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;DirectRuntimeWitness witness={0};witness.mode=mode;
    XrXirProgram *program=NULL;allocation_request_begin(&allocation_prefix_requests);allocation_calls=0;
    CHECK(direct_runtime_seal(&witness,&program)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("direct",0,mode,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!invocation_direct_owner_run(mode,NULL,NULL));allocation_phase_compile_end(&map,ordinal);
    }
    witness=(DirectRuntimeWitness){0};witness.mode=mode;allocation_calls=0;
    CHECK(direct_runtime_seal(&witness,&program)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(invocation_direct_execute(program,&witness,NULL,NULL) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!witness.releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!invocation_direct_owner_run(mode,NULL,NULL));allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!invocation_direct_execute(program,&witness,NULL,NULL));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!witness.releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1);allocation_phase_complete(&map);
}

static void invocation_capture_allocation_phase_case(unsigned mode,bool scalar) {
    allocation_phase_case_begin("capture",scalar,mode);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(invocation_capture_owner_run(mode,scalar,NULL,NULL) && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;CaptureRuntimeWitness witness={0};witness.mode=mode;witness.scalar=scalar;
    XrXirProgram *program=NULL;allocation_request_begin(&allocation_prefix_requests);allocation_calls=0;
    CHECK(capture_runtime_seal(&witness,&program)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("capture",scalar,mode,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!invocation_capture_owner_run(mode,scalar,NULL,NULL));allocation_phase_compile_end(&map,ordinal);
    }
    witness=(CaptureRuntimeWitness){0};witness.mode=mode;witness.scalar=scalar;allocation_calls=0;
    CHECK(capture_runtime_seal(&witness,&program)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(invocation_capture_execute(program,&witness,NULL,NULL) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!witness.releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!invocation_capture_owner_run(mode,scalar,NULL,NULL));allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!invocation_capture_execute(program,&witness,NULL,NULL));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!witness.releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1);allocation_phase_complete(&map);
}

static void invocation_return_capture_allocation_phase_case(unsigned mode,bool scalar) {
    allocation_phase_case_begin("return_capture",scalar,mode);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(invocation_return_capture_owner_run(mode,scalar,NULL,NULL) && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;ReturnedCaptureRuntimeWitness witness={0};witness.mode=mode;witness.scalar=scalar;
    XrXirProgram *program=NULL;allocation_request_begin(&allocation_prefix_requests);allocation_calls=0;
    CHECK(returned_capture_runtime_seal(&witness,&program)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("return_capture",scalar,mode,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!invocation_return_capture_owner_run(mode,scalar,NULL,NULL));allocation_phase_compile_end(&map,ordinal);
    }
    witness=(ReturnedCaptureRuntimeWitness){0};witness.mode=mode;witness.scalar=scalar;allocation_calls=0;
    CHECK(returned_capture_runtime_seal(&witness,&program)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(invocation_return_capture_execute(program,&witness,NULL,NULL) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!witness.releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!invocation_return_capture_owner_run(mode,scalar,NULL,NULL));allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!invocation_return_capture_execute(program,&witness,NULL,NULL));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!witness.releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1);allocation_phase_complete(&map);
}

static void invocation_function_cell_allocation_phase_case(unsigned mode,bool scalar) {
    allocation_phase_case_begin("function_cell",scalar,mode);
    allocation_request_begin(&allocation_whole_requests);
    allocation_calls=0;fail_at=SIZE_MAX;CHECK(invocation_function_cell_owner_run(mode,scalar,NULL,NULL) && !live);
    allocation_request_end();
    const size_t whole=allocation_calls;FunctionCellRuntimeWitness witness={0};witness.mode=mode;witness.scalar=scalar;
    XrXirProgram *program=NULL;allocation_request_begin(&allocation_prefix_requests);allocation_calls=0;
    CHECK(function_cell_runtime_seal(&witness,&program)==XR_XIR_OK);
    allocation_request_end();
    const size_t prefix_calls=allocation_calls;
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1 && !live);program=NULL;
    AllocationPhaseMap map=allocation_phase_map("function_cell",scalar,mode,whole,prefix_calls);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_COMPILER) && ordinal<map.prefix;++ordinal) {
        allocation_phase_compile_begin(&map,ordinal);
        CHECK(!invocation_function_cell_owner_run(mode,scalar,NULL,NULL));allocation_phase_compile_end(&map,ordinal);
    }
    witness=(FunctionCellRuntimeWitness){0};witness.mode=mode;witness.scalar=scalar;allocation_calls=0;
    CHECK(function_cell_runtime_seal(&witness,&program)==XR_XIR_OK && allocation_calls==map.prefix);
    AllocationPhasePhysical physical=allocation_phase_physical();
    XrCompileResourceStats prefix=native_fixture_stats(&program->context);
    allocation_stage="runtime-baseline";
    allocation_request_begin(&allocation_runtime_requests);
    allocation_calls=0;CHECK(xr_xir_compile_program_retain(program));
    CHECK(invocation_function_cell_execute(program,&witness,NULL,NULL) && allocation_calls==map.runtime);
    allocation_request_end();
    allocation_request_equal(&allocation_whole_requests,map.prefix,&allocation_runtime_requests,0,map.runtime);
    allocation_phase_restore(program,physical,&prefix);CHECK(!witness.releases);
    for(size_t ordinal=0;(allocation_phase_selection&ALLOCATION_PHASE_RUNTIME) && ordinal<map.runtime;++ordinal) {
        if(allocation_phase_selection&ALLOCATION_PHASE_BRIDGE) {
            allocation_stage="original-runtime-bridge";
            allocation_request_begin(&allocation_reference_requests);
            allocation_calls=0;fail_at=map.prefix+ordinal;
            CHECK(!invocation_function_cell_owner_run(mode,scalar,NULL,NULL));allocation_request_end();fail_at=SIZE_MAX;
            CHECK(live==physical.blocks && class_live_bytes==physical.bytes);
            allocation_request_equal(&allocation_whole_requests,0,&allocation_reference_requests,0,map.prefix);
            ++allocation_phase_bridge_ordinals;
        }
        allocation_phase_runtime_begin(&map,ordinal,program);
        CHECK(!invocation_function_cell_execute(program,&witness,NULL,NULL));
        allocation_phase_runtime_end(&map,ordinal,program,physical,&prefix);CHECK(!witness.releases);
    }
    allocation_stage="last-owner-drop";
    xr_xir_compile_program_drop(program);CHECK(witness.releases==1);allocation_phase_complete(&map);
}
#endif // XIR_ALLOCATION_PHASE_CASES_H
