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
