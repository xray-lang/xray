/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_provider_aot_sources_gen.inc.c - Declared native adapter source
 *
 * Generated from explicit provider declarations. Do not edit.
 */

/* clang-format off */

static const XrAotNativeProviderSource xr_aot_native_provider_sources[] = {
    {
        .contract_id = { { 0x17, 0x07, 0xc5, 0x61, 0xe4, 0x98, 0xd7, 0x3d, 0x03, 0x86, 0x23, 0x30, 0xb7, 0xcd, 0xae, 0xc6 } },
        .operation_id = { { 0x48, 0x83, 0x2a, 0x90, 0x67, 0x12, 0x89, 0x24, 0x47, 0xd7, 0x3f, 0xa2, 0x84, 0xa0, 0x74, 0x0c } },
        .kind = XR_AOT_NATIVE_I64_NULLARY,
        .header = "os/os_time.h",
        .definition =
            "_Static_assert(_Generic(&xr_time_monotonic_ns,\n"
            "    uint64_t (*)(void): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_0(void *context, int64_t *result_out) {\n"
            "    (void)context;\n"
            "    if (!result_out) return 1;\n"
            "    uint64_t raw = xr_time_monotonic_ns();\n"
            "    *result_out = raw <= INT64_MAX ? (int64_t)raw :\n"
            "                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);\n"
            "    return 0;\n"
            "}\n"
            "\n"
        ,
    },
    {
        .contract_id = { { 0x17, 0x07, 0xc5, 0x61, 0xe4, 0x98, 0xd7, 0x3d, 0x03, 0x86, 0x23, 0x30, 0xb7, 0xcd, 0xae, 0xc6 } },
        .operation_id = { { 0xa8, 0x46, 0xda, 0xbb, 0x8b, 0x04, 0x1d, 0x87, 0x50, 0x24, 0xea, 0x11, 0x4e, 0xce, 0xec, 0x5b } },
        .kind = XR_AOT_NATIVE_I64_NULLARY,
        .header = "os/os_time.h",
        .definition =
            "_Static_assert(_Generic(&xr_time_realtime_ns,\n"
            "    uint64_t (*)(void): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_1(void *context, int64_t *result_out) {\n"
            "    (void)context;\n"
            "    if (!result_out) return 1;\n"
            "    uint64_t raw = xr_time_realtime_ns();\n"
            "    *result_out = raw <= INT64_MAX ? (int64_t)raw :\n"
            "                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);\n"
            "    return 0;\n"
            "}\n"
            "\n"
        ,
    },
    {
        .contract_id = { { 0x17, 0x07, 0xc5, 0x61, 0xe4, 0x98, 0xd7, 0x3d, 0x03, 0x86, 0x23, 0x30, 0xb7, 0xcd, 0xae, 0xc6 } },
        .operation_id = { { 0xa9, 0x64, 0xbc, 0x65, 0x4c, 0x1c, 0xfa, 0xec, 0xdd, 0x3c, 0xf2, 0x76, 0xaf, 0xae, 0xb3, 0x9a } },
        .kind = XR_AOT_NATIVE_I64_UNARY,
        .header = "shared/xr_time_offset.h",
        .definition =
            "_Static_assert(_Generic(&xr_time_utc_offset_at,\n"
            "    bool (*)(int64_t, int64_t *): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_2(void *context, int64_t argument, int64_t *result_out) {\n"
            "    (void)context;\n"
            "    return xr_time_utc_offset_at(argument, result_out) ?\n"
            "           0 : 1;\n"
            "}\n"
            "\n"
        ,
    },
    {
        .contract_id = { { 0x17, 0x07, 0xc5, 0x61, 0xe4, 0x98, 0xd7, 0x3d, 0x03, 0x86, 0x23, 0x30, 0xb7, 0xcd, 0xae, 0xc6 } },
        .operation_id = { { 0xfb, 0x7d, 0x08, 0x64, 0x85, 0xbe, 0x51, 0x21, 0xd5, 0xf2, 0xe4, 0x23, 0xd8, 0xfb, 0xcf, 0xd9 } },
        .kind = XR_AOT_NATIVE_I64_NULLARY,
        .header = "os/os_time.h",
        .definition =
            "_Static_assert(_Generic(&xr_time_process_cpu_ns,\n"
            "    uint64_t (*)(void): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_3(void *context, int64_t *result_out) {\n"
            "    (void)context;\n"
            "    if (!result_out) return 1;\n"
            "    uint64_t raw = xr_time_process_cpu_ns();\n"
            "    *result_out = raw <= INT64_MAX ? (int64_t)raw :\n"
            "                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);\n"
            "    return 0;\n"
            "}\n"
            "\n"
        ,
    },
    {
        .contract_id = { { 0x23, 0x5a, 0x1c, 0x0a, 0x68, 0x22, 0xfd, 0xde, 0xc5, 0xcb, 0x75, 0xcf, 0x87, 0x6a, 0x1e, 0xc3 } },
        .operation_id = { { 0x01, 0x2b, 0x0f, 0x73, 0x60, 0xee, 0xe2, 0x31, 0xd5, 0x81, 0x34, 0x46, 0x89, 0xc6, 0x20, 0x9f } },
        .kind = XR_AOT_NATIVE_I64_NULLARY,
        .header = "shared/xr_os_core.h",
        .definition =
            "_Static_assert(_Generic(&xr_os_core_getpid,\n"
            "    int64_t (*)(void): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_4(void *context, int64_t *result_out) {\n"
            "    (void)context;\n"
            "    if (!result_out) return 1;\n"
            "    *result_out = xr_os_core_getpid();\n"
            "    return 0;\n"
            "}\n"
            "\n"
        ,
    },
    {
        .contract_id = { { 0xe2, 0x42, 0xb0, 0x07, 0x19, 0xd6, 0xcc, 0xd9, 0xbc, 0xd4, 0x0f, 0x8b, 0x34, 0x33, 0x6a, 0xf2 } },
        .operation_id = { { 0x8e, 0x76, 0x5f, 0x2e, 0xe5, 0x84, 0x9b, 0xc3, 0x9d, 0xfc, 0xf0, 0xd3, 0xb1, 0x2b, 0x5e, 0x0d } },
        .kind = XR_AOT_NATIVE_BOOL_I64_UNARY,
        .header = "os/os_pipe.h",
        .definition =
            "_Static_assert(_Generic(&xr_pipe_close,\n"
            "    int (*)(XrPipeHandle): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_5(void *context, int64_t argument, bool *result_out) {\n"
            "    (void)context;\n"
            "    if (!result_out) return 1;\n"
            "    XrPipeHandle handle = (XrPipeHandle)argument;\n"
            "    if ((int64_t)handle != argument) {\n"
            "        *result_out = false;\n"
            "        return 0;\n"
            "    }\n"
            "    *result_out = xr_pipe_close(handle) == 0;\n"
            "    return 0;\n"
            "}\n"
            "\n"
        ,
    },
    {
        .contract_id = { { 0xe2, 0x42, 0xb0, 0x07, 0x19, 0xd6, 0xcc, 0xd9, 0xbc, 0xd4, 0x0f, 0x8b, 0x34, 0x33, 0x6a, 0xf2 } },
        .operation_id = { { 0xc1, 0x87, 0xd1, 0x40, 0x01, 0x17, 0x9d, 0xc6, 0x1f, 0x90, 0xde, 0x19, 0xe1, 0x20, 0x4b, 0x28 } },
        .kind = XR_AOT_NATIVE_OPTIONAL_I64_PAIR_NULLARY,
        .header = "os/os_pipe.h",
        .definition =
            "_Static_assert(_Generic(&xr_pipe_create,\n"
            "    int (*)(XrPipe *, const XrPipeOptions *): 1, default: 0),\n"
            "    \"Provider host signature does not match its explicit adapter\");\n"
            "static int xr_aot_native_provider_6(void *context, bool *present_out, int64_t *first_out, int64_t *second_out) {\n"
            "    (void)context;\n"
            "    if (!present_out || !first_out || !second_out)\n"
            "        return 1;\n"
            "    XrPipe pipe = {XR_PIPE_INVALID, XR_PIPE_INVALID};\n"
            "    bool present = xr_pipe_create(&pipe, NULL) == 0;\n"
            "    *present_out = present;\n"
            "    *first_out = present ? (int64_t)pipe.read : 0;\n"
            "    *second_out = present ? (int64_t)pipe.write : 0;\n"
            "    return 0;\n"
            "}\n"
            "\n"
        ,
    },
};
#define XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT 7u

/* clang-format on */
