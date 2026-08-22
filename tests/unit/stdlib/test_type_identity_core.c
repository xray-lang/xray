/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "../test_framework.h"
#include "shared/xr_type_identity_core.h"

TEST(type_identity_core_pins_public_ids) {
    ASSERT_EQ_INT(xr_type_identity_core_eval(
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO, XR_SEM_CONSUMER_RUNTIME,
                      XR_TYPE_IDENTITY_CORE_NULL),
                  0);
    ASSERT_EQ_INT(xr_type_identity_core_eval(
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO, XR_SEM_CONSUMER_VM,
                      XR_TYPE_IDENTITY_CORE_BOOL),
                  1);
    ASSERT_EQ_INT(xr_type_identity_core_eval(
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO,
                      XR_SEM_CONSUMER_AOT_HOSTED, XR_TYPE_IDENTITY_CORE_I64),
                  8);
    ASSERT_EQ_INT(xr_type_identity_core_eval(
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO,
                      XR_SEM_CONSUMER_AOT_FREESTANDING, XR_TYPE_IDENTITY_CORE_F64),
                  11);
    ASSERT_EQ_INT(xr_type_identity_core_eval(
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO, XR_SEM_CONSUMER_CGEN,
                      XR_TYPE_IDENTITY_CORE_OBJECT),
                  18);
    ASSERT_EQ_INT(xr_type_identity_core_eval(
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO,
                      XR_SEM_CONSUMER_SEMANTIC_PLAN, XR_TYPE_IDENTITY_CORE_RUNE),
                  43);
}

TEST(type_identity_core_covers_every_kind) {
    static const XrTypeIdentityCoreKind kinds[] = {
        XR_TYPE_IDENTITY_CORE_NULL,
        XR_TYPE_IDENTITY_CORE_BOOL,
        XR_TYPE_IDENTITY_CORE_I64,
        XR_TYPE_IDENTITY_CORE_F64,
        XR_TYPE_IDENTITY_CORE_STRING,
        XR_TYPE_IDENTITY_CORE_FUNCTION,
        XR_TYPE_IDENTITY_CORE_ARRAY,
        XR_TYPE_IDENTITY_CORE_SET,
        XR_TYPE_IDENTITY_CORE_MAP,
        XR_TYPE_IDENTITY_CORE_INSTANCE,
        XR_TYPE_IDENTITY_CORE_OBJECT,
        XR_TYPE_IDENTITY_CORE_BIGINT,
        XR_TYPE_IDENTITY_CORE_STRINGBUILDER,
        XR_TYPE_IDENTITY_CORE_CHANNEL,
        XR_TYPE_IDENTITY_CORE_REGEX,
        XR_TYPE_IDENTITY_CORE_DATETIME,
        XR_TYPE_IDENTITY_CORE_PANIC_INFO,
        XR_TYPE_IDENTITY_CORE_ENUM_VALUE,
        XR_TYPE_IDENTITY_CORE_ENUM_TYPE,
        XR_TYPE_IDENTITY_CORE_BOUND_METHOD,
        XR_TYPE_IDENTITY_CORE_ITERATOR,
        XR_TYPE_IDENTITY_CORE_MODULE,
        XR_TYPE_IDENTITY_CORE_COROUTINE,
        XR_TYPE_IDENTITY_CORE_RANGE,
        XR_TYPE_IDENTITY_CORE_TASK,
        XR_TYPE_IDENTITY_CORE_NETCONN,
        XR_TYPE_IDENTITY_CORE_NETLISTENER,
        XR_TYPE_IDENTITY_CORE_ATOMIC,
        XR_TYPE_IDENTITY_CORE_WORKQUEUE,
        XR_TYPE_IDENTITY_CORE_RESULTGROUP,
        XR_TYPE_IDENTITY_CORE_COUNTDOWNLATCH,
        XR_TYPE_IDENTITY_CORE_SEMAPHORE,
        XR_TYPE_IDENTITY_CORE_EVENTCOUNT,
        XR_TYPE_IDENTITY_CORE_THREAD,
        XR_TYPE_IDENTITY_CORE_BUFFER,
        XR_TYPE_IDENTITY_CORE_RUNE,
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        ASSERT_EQ_INT(xr_type_identity_core_eval_impl(kinds[i]), (int) kinds[i]);
    ASSERT_EQ_INT(xr_type_identity_core_eval_impl((XrTypeIdentityCoreKind) 255), UINT8_MAX);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Type Identity Core");
RUN_TEST(type_identity_core_pins_public_ids);
RUN_TEST(type_identity_core_covers_every_kind);

TEST_MAIN_END()
