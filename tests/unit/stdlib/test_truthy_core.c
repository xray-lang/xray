/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_truthy_core.c - Unit tests for runtime-neutral truthiness rules
 */

#include "../test_framework.h"
#include "shared/xr_truthy_core.h"
#include <math.h>

#define TRUTHY(kind, integer, floating, size)                                                       \
    xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,                                      \
                        XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO, XR_SEM_CONSUMER_RUNTIME, (kind),       \
                        (integer), (floating), (size))

TEST(truthy_core_scalars_match_language) {
    ASSERT(!TRUTHY(XR_TRUTHY_CORE_NULL, 0, 0.0, 0));
    ASSERT(!TRUTHY(XR_TRUTHY_CORE_BOOL, 0, 0.0, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_BOOL, 1, 0.0, 0));
    ASSERT(!TRUTHY(XR_TRUTHY_CORE_INT, 0, 0.0, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_INT, 1, 0.0, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_INT, -1, 0.0, 0));
    ASSERT(!TRUTHY(XR_TRUTHY_CORE_FLOAT, 0, 0.0, 0));
    ASSERT(!TRUTHY(XR_TRUTHY_CORE_FLOAT, 0, -0.0, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_FLOAT, 0, 0.25, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_FLOAT, 0, -0.25, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_FLOAT, 0, NAN, 0));
}

TEST(truthy_core_sized_values_use_length) {
    ASSERT(!TRUTHY(XR_TRUTHY_CORE_SIZED, 0, 0.0, 0));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_SIZED, 0, 0.0, 1));
    ASSERT(TRUTHY(XR_TRUTHY_CORE_SIZED, 0, 0.0, -1));
}

TEST(truthy_core_unknown_objects_are_truthy) {
    ASSERT(TRUTHY(XR_TRUTHY_CORE_OBJECT, 0, 0.0, 0));
    ASSERT(!TRUTHY((XrTruthyCoreKind) 255, 0, 0.0, 0));
}

TEST(truthy_owner_declares_every_production_consumer) {
    ASSERT(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                          XR_SEM_CONSUMER_SEMANTIC_PLAN));
    ASSERT(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                          XR_SEM_CONSUMER_RUNTIME));
    ASSERT(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                          XR_SEM_CONSUMER_VM));
    ASSERT(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                          XR_SEM_CONSUMER_AOT_HOSTED));
    ASSERT(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                          XR_SEM_CONSUMER_AOT_FREESTANDING));
    ASSERT(xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO,
                                          XR_SEM_CONSUMER_CGEN));
    ASSERT(xr_semantic_owner_cgen_adapter(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                                          XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO) != NULL);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Truthy Core");
RUN_TEST(truthy_core_scalars_match_language);
RUN_TEST(truthy_core_sized_values_use_length);
RUN_TEST(truthy_core_unknown_objects_are_truthy);
RUN_TEST(truthy_owner_declares_every_production_consumer);

TEST_MAIN_END()

#undef TRUTHY
