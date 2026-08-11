/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * target_profile_test_fixture.h - Structured TargetProfile test authority
 */

#ifndef TARGET_PROFILE_TEST_FIXTURE_H
#define TARGET_PROFILE_TEST_FIXTURE_H

#include "../../../src/plan/target/xr_target_profile.h"

typedef struct XrTestTargetProfileFixture {
    XrRuntimeAbiContract runtime_abi;
    XrRuntimeObjectHeaderMaterializationFacts object_header_materialization;
    XrTargetProviderContract providers[2];
    XrTargetProfileBuildInput input;
} XrTestTargetProfileFixture;

bool xr_test_target_profile_fixture_init(XrTestTargetProfileFixture *fixture,
                                         bool ilp32, uint8_t runtime_profile);
XrTargetProfile *xr_test_target_profile_build(bool ilp32,
                                              uint8_t runtime_profile);

#endif  // TARGET_PROFILE_TEST_FIXTURE_H
