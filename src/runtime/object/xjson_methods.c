/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_methods.c - Json instance method bodies + dispatch table.
 */

#include "xjson_methods.h"
#include "xjson.h"
#include "xiterator.h"
#include "xstring.h"
#include "../value/xvalue.h"
#include "../value/xvalue_format.h"
#include "../symbol/xsymbol_table.h"
#include "../../coro/xcoroutine.h"
#include "../../base/xchecks.h"

static inline XrJson *json_self(XrValue self) {
    XR_DCHECK(xr_value_is_json(self), "json method: receiver is not Json");
    return (XrJson *)XR_TO_PTR(self);
}

/* Internal protocol used by `for (k, v in obj)` lowering. */
static XrValue xr_json_method_entries_iterator(XrayIsolate *iso, XrValue self,
                                               XrValue *args, int argc) {
    (void)args; (void)argc;
    XrCoroutine *coro = xr_current_coro(iso);
    XrIterator *iter = xr_iterator_new_from_json(coro, json_self(self), iso);
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

static XrValue xr_json_method_to_string(XrayIsolate *iso, XrValue self,
                                        XrValue *args, int argc) {
    (void)args; (void)argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

const XrMethodSlot xr_json_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_ENTRIES_ITERATOR] = { xr_json_method_entries_iterator, 0, 0, 0 },
    [SYMBOL_TOSTRING]         = { xr_json_method_to_string,        0, 0,
                                   XR_METHOD_FLAG_MAY_THROW },
};
