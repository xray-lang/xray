/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_time_helpers.inc - AOT stdlib time module recognition
 */

static bool cg_is_time_sleep_call(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs != 2)
        return false;
    const char *method = (const char *) v->aux;
    if (!method || strcmp(method, "sleep") != 0)
        return false;
    return cg_value_is_module_import(f, v->args[0], "time");
}

static bool cg_is_time_module_call(const XiFunc *f, const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1)
        return false;
    return cg_value_is_module_import(f, v->args[0], "time");
}

static const char *cg_time_module_helper(const XiFunc *f, const XiValue *v) {
    if (!cg_is_time_module_call(f, v) || v->nargs != 1)
        return NULL;
    const char *method = (const char *) v->aux;
    if (!method)
        return NULL;
    if (strcmp(method, "now") == 0)
        return "xr_aot_time_now";
    if (strcmp(method, "monotonic") == 0)
        return "xr_aot_time_monotonic";
    if (strcmp(method, "nanos") == 0)
        return "xr_aot_time_nanos";
    if (strcmp(method, "micros") == 0)
        return "xr_aot_time_micros";
    if (strcmp(method, "clock") == 0)
        return "xr_aot_time_clock";
    return NULL;
}
