/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_module_fixture.h - Dependency-ordered diamond initialization fixture
 */

#ifndef XR_PROGRAM_MODULE_FIXTURE_H
#define XR_PROGRAM_MODULE_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"
#include <string.h>

typedef struct XrProgramModuleFixture {
    XrCoreIrKey profile;
    uint16_t feature;
    XrCoreIrInstructionInput returns[4];
    XrCoreIrBlockInput blocks[4];
    XrCoreIrFunctionInput functions[4];
    XrCoreIrKey dependencies[4][2];
    XrCoreIrModuleInput modules[4];
    XrCoreIrModuleSlotInput slots[4][2];
    XrCoreIrProgramInput input;
} XrProgramModuleFixture;

static inline void xr_program_module_fixture_init(XrProgramModuleFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *module_names[] = {"base", "left", "right", "main"};
    const char *function_names[] = {"base:init", "left:init", "right:init", "main:init"};
    for (uint32_t index = 0u; index < 4u; ++index) {
        fixture->returns[index].operation_id = XR_CORE_OP_CORE_RETURN;
        fixture->blocks[index] = (XrCoreIrBlockInput) {
            .key = xr_core_ir_key(function_names[index], strlen(function_names[index])),
            .instructions = &fixture->returns[index],
            .instruction_count = 1u,
        };
        fixture->functions[index] = (XrCoreIrFunctionInput) {
            .key = xr_core_ir_key(function_names[index], strlen(function_names[index])),
            .entry_block = fixture->blocks[index].key,
            .blocks = &fixture->blocks[index], .block_count = 1u,
            .flags = index == 3u ? XR_PROGRAM_FUNCTION_ENTRY : 0u,
        };
        fixture->modules[index] = (XrCoreIrModuleInput) {
            .key = xr_core_ir_key(module_names[index], strlen(module_names[index])),
            .functions = &fixture->functions[index], .function_count = 1u,
            .initializer = fixture->functions[index].key,
            .dependencies = index ? fixture->dependencies[index] : NULL,
            .dependency_count = index == 3u ? 2u : index ? 1u : 0u,
        };
    }
    fixture->dependencies[1][0] = fixture->modules[0].key;
    fixture->dependencies[2][0] = fixture->modules[0].key;
    fixture->dependencies[3][0] = fixture->modules[1].key;
    fixture->dependencies[3][1] = fixture->modules[2].key;
    static const char profile[] = "module-initialization-profile";
    fixture->profile = xr_core_ir_key(profile, sizeof(profile) - 1u);
    fixture->feature = XR_CORE_FEATURE_CORE_BASE;
    fixture->input = (XrCoreIrProgramInput) {
        .semantic_profile_fingerprint = fixture->profile.bytes,
        .required_features = &fixture->feature, .required_feature_count = 1u,
        .modules = fixture->modules, .module_count = 4u,
    };
}

static inline void xr_program_module_fixture_add_slots(XrProgramModuleFixture *fixture) {
    for (uint32_t module = 0u; module < 4u; ++module) {
        fixture->slots[module][0] = (XrCoreIrModuleSlotInput) {
            .key = xr_core_ir_key("counter:1", 9u), .type_id = XR_CORE_TYPE_I64,
        };
        fixture->slots[module][1] = (XrCoreIrModuleSlotInput) {
            .key = xr_core_ir_key("label:2", 7u), .type_id = XR_CORE_TYPE_STRING,
            .flags = XR_PROGRAM_MODULE_SLOT_CONST,
        };
        fixture->modules[module].slots = fixture->slots[module];
        fixture->modules[module].slot_count = 2u;
    }
}

#endif  // XR_PROGRAM_MODULE_FIXTURE_H
