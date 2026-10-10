/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_native.c - Generated native programs with no VM linkage
 *
 * KEY CONCEPT:
 *   Runtime-only execution checks fixed expectations without a VM oracle.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_consumer_context_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_module_forest_cases.h"
#include "xir_module_forest_seal_cases.h"
extern const XrXirProgramSpec forest_program;
#include "xir_program_cases.h"
#include "xir_capture_cases.h"
#include "xir_array_program_cases.h"
#include "xir_nominal_generic_cases.h"
#include "xir_nominal_expression_cases.h"
#include "xir_nominal_visibility_cases.h"
#include "xir_nominal_transport_cases.h"
#include "xir_struct_ops_cases.h"
#include "xir_enum_ops_cases.h"
extern const XrXirProgramSpec enum_ops_program, enum_wrong_program;
#include "xir_struct_set_cases.h"
#include "xir_cleanup_program_cases.h"
#include "xir_path_program_cases.h"
extern const XrXirProgramSpec path0_program, path1_program, path2_program;
extern const XrXirProgramSpec cleanup0_program, cleanup1_program, cleanup2_program, cleanup3_program, cleanup4_program, cleanup5_program;
extern const XrXirProgramSpec struct_set_program;
extern const XrXirProgramSpec struct_ops_program;
extern const XrXirProgramSpec nominal_transport_program;
extern const XrXirProgramSpec nominal_generic_program;
extern const XrXirProgramSpec nominal_expression_program;
extern const XrXirProgramSpec array_program0_program, array_program1_program, array_program2_program;
extern const XrXirProgramSpec captures_program, captures_error_program;
extern const XrXirProgramSpec program0_program, program1_program, program2_program;
extern const XrXirProgramSpec nominal0_program, nominal3_program, nominal_chain_program;
extern const XrXirProgramSpec enum_metadata_program;
static void enum_metadata_descriptors(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &enum_metadata_program, &program) == XR_XIR_OK);
    xr_xir_compile_program_drop(program); program = NULL;
    XrXirNominalIdentity identity = enum_metadata_program.types->nominals->identities[0];
    CHECK(identity.kind == XR_XIR_NOMINAL_ENUM && identity.variant_count == 3);
    CHECK(identity.variants[0].field_count == 0 && identity.variants[2].field_begin == 1);
    XrXirNominalVariant variants[3]; memcpy(variants, identity.variants, sizeof(variants));
    identity.variants = variants;
    XrXirNominalTable table = {NULL, 1, &identity};
    XrXirTypes types = *enum_metadata_program.types; types.nominals = &table;
    XrXirProgramSpec spec = enum_metadata_program; spec.types = &types;
    variants[0].name = (XrXirLiteral) {"Other", 5};
    CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_BAD_STRUCTURE && !program);
    variants[0] = enum_metadata_program.types->nominals->identities[0].variants[0];
    spec.abi_version = 13;
    CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_BAD_LAYOUT && !program);
    spec.abi_version = 16;
    CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_BAD_LAYOUT && !program);
    spec.abi_version = 20;
    CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_BAD_LAYOUT && !program);
    spec.abi_version = XR_XIR_PROGRAM_ABI_VERSION;
    spec.target.abi_version = 11;
    CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_BAD_LAYOUT && !program);
}
int main(void) {
    consumer_context=consumer_context_default();
    XrXirProgram *forest = module_forest_seal(&forest_program);
    module_forest_cases(forest);
    const XrXirProgramSpec *paths[] = {&path0_program,&path1_program,&path2_program};
    for (unsigned kind = 0; kind < 3; ++kind) {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, paths[kind], &program) == XR_XIR_OK);
        path_program_cases(program,kind);
        CHECK(!runtime_live && !runtime_bytes);
    }
    const XrXirProgramSpec *cleanup_specs[] = {&cleanup0_program, &cleanup1_program, &cleanup2_program,
        &cleanup3_program, &cleanup4_program, &cleanup5_program};
    for (unsigned mode = 0; mode < 6; ++mode) {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, cleanup_specs[mode], &program) == XR_XIR_OK);
        cleanup_program_cases(program, mode);
        CHECK(!runtime_live && !runtime_bytes);
    }

    XrXirProgram *enum_program = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &enum_ops_program, &enum_program) == XR_XIR_OK);
    enum_ops_cases(enum_program); CHECK(runtime_live == 0 && runtime_bytes == 0);
    enum_program = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &enum_wrong_program, &enum_program) == XR_XIR_OK);
    enum_wrong_variant_cases(enum_program); CHECK(runtime_live == 0 && runtime_bytes == 0);
    enum_metadata_descriptors();
    nominal_visibility_cases(&nominal0_program);
    XrXirProgram *set_program = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &struct_set_program, &set_program) == XR_XIR_OK);
    struct_set_cases(set_program);
    XrXirProgram *struct_program = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &struct_ops_program, &struct_program) == XR_XIR_OK);
    struct_ops_cases(struct_program);
    XrXirProgram *transport = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &nominal_transport_program, &transport) == XR_XIR_OK);
    XrXirNominalIdentity hidden = nominal_transport_program.types->nominals->identities[0];
    hidden.module = (XrXirLiteral) {"missing",7};
    XrXirNominalTable hidden_table = {NULL,1,&hidden};
    XrXirTypes hidden_types = *nominal_transport_program.types; hidden_types.nominals = &hidden_table;
    XrXirProgramSpec invalid = nominal_transport_program; invalid.types = &hidden_types; XrXirProgram *rejected = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &invalid, &rejected) == XR_XIR_BAD_STRUCTURE && !rejected);
    XrXirInstance *transport_instances[6];
    for (unsigned i = 0; i < 6; ++i) transport_instances[i] = nominal_transport_instance(transport);
    const XrXirTypes *transport_types = transport->types;
    xr_xir_compile_program_drop(transport); transport = NULL;
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned branch = 0; branch < 2; ++branch) {
        XrXirValue escaped = nominal_transport_cases(transport_instances[mode * 2 + branch],
            transport_types,mode,branch != 0);
        nominal_transport_escaped(&escaped);
    }
    CHECK(!runtime_live && !runtime_bytes);
    XrXirProgram *combined = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &nominal_generic_program, &combined) == XR_XIR_OK);
    nominal_generic_cases(combined);
    XrXirProgram *expressions = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, &nominal_expression_program, &expressions) == XR_XIR_OK);
    nominal_expression_cases(expressions);
    const XrXirProgramSpec *nominals[] = {&nominal0_program, &nominal3_program, &nominal_chain_program};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(nominals[i]->types && nominals[i]->types->nominals);
        if (i == 2) {
            XrXirCompileContext budget=consumer_context_limits((XrCompileResourceLimits){65536,65536,10001});budget.limits.parameters=1000;
            const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
            XrXirLayout layout = {0}; uint32_t offsets[2] = {0};
            CHECK(xr_xir_compile_nominal_layout(&budget, nominals[i]->types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_OK);
            CHECK(layout.size == 64 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 32);
        }
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, nominals[i], &program) == XR_XIR_OK);
        program_cases(program, 0);
    }
    const XrXirProgramSpec *arrays[] = {&array_program0_program,&array_program1_program,&array_program2_program};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, arrays[mode], &program) == XR_XIR_OK);
        array_program_cases(program,mode == 1);
    }
    const XrXirProgramSpec *specs[] = {&program0_program, &program1_program, &program2_program};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, specs[mode], &program) == XR_XIR_OK);
        program_cases(program, mode);
    }
    for (unsigned throwing = 0; throwing < 2; ++throwing) {
    XrXirProgram *captures = NULL;
    CHECK(xr_xir_compile_program_seal(suite_context, throwing ? &captures_error_program : &captures_program, &captures) == XR_XIR_OK);
    capture_cases(captures,throwing != 0);
    }
    puts("Native capture environments, two suspensions, cancellation and escaped ownership passed");
    puts("Native module programs match independent output, state and lifetime expectations");
    CHECK(!runtime_live && !runtime_bytes);
    consumer_contexts_free();
    return 0;
}
