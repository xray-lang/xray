/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_receiver_alias.c - see xi_receiver_alias.h
 */

#include "xi_receiver_alias.h"
#include "xi_own.h"

XR_FUNC bool xi_call_result_aliases_receiver(const XiValue *v) {
    if (!v || v->nargs < 1 || !v->args[0] || v->result_alias_operand != 0)
        return false;
    return xi_own_type_is_rc(v->type);
}
