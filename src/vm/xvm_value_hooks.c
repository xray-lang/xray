/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_value_hooks.c - VM implementations of the Map/Set instance hash/eq hooks
 *
 * A class that implements Hashable by hand — declaring `hash() -> int` and
 * `operator ==` — must key a Map or Set by value. xr_hash_value / xr_value_eq
 * cannot run user methods on their own, so they defer to these hooks. The VM
 * installs them once at isolate init (xr_vm_install_value_hooks); each reaches
 * the running isolate through the thread-local execution context and invokes
 * the method reentrantly. The coro heap never moves objects, so the borrowed
 * key / entry pointers held by the caller stay valid across the call.
 */

#include "xvm_internal.h"

#include "../runtime/class/xclass.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/core/xr_exec_context.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/value/xvalue_hash.h"

/* Instance method closure by symbol, or NULL when the class does not declare a
 * user-defined (closure/operator) method of that name. */
static XrClosure *hook_instance_method(XrValue v, int symbol) {
    if (symbol < 0 || !XR_IS_PTR(v) || v.heap_type != XR_TINSTANCE || !v.ptr)
        return NULL;
    XrInstance *inst = (XrInstance *) v.ptr;
    XrClass *cls = inst->klass;
    if (!cls)
        return NULL;
    XrMethod *m = xr_class_lookup_method(cls, symbol);
    if (!m || (m->type != XMETHOD_CLOSURE && m->type != XMETHOD_OPERATOR) || !m->as.closure)
        return NULL;
    return m->as.closure;
}

static int hook_symbol(XrVMRuntime *iso, const char *name) {
    XrSymbolTable *t = (iso && iso->core_rt) ? (XrSymbolTable *) iso->core_rt->symbol_table : NULL;
    return t ? xr_symbol_lookup_in_table(t, name) : -1;
}

/* hash(): invoke the user method and return its int as the key hash. A throw
 * leaves the isolate's pending error set; we return a deterministic hash so the
 * map/set operation finishes and the enclosing opcode unwinds on that error. */
static bool vm_instance_hash_hook(XrValue key, uint32_t *out_hash) {
    XrVMRuntime *iso = xr_exec_context_vm_owner();
    if (!iso)
        return false;
    XrClosure *closure = hook_instance_method(key, hook_symbol(iso, "hash"));
    if (!closure)
        return false;
    XrValue result = xr_vm_call_closure(iso, closure, &key, 1);
    *out_hash = XR_IS_INT(result) ? (uint32_t) XR_TO_INT(result) : 1u;
    return true;
}

/* operator ==: governs equality when the left operand's class defines it, which
 * is exactly when the language treats == as user-defined. Returns -1 to defer to
 * the default reference / @derive(Eq) comparison. */
static int vm_instance_eq_hook(XrValue a, XrValue b) {
    XrVMRuntime *iso = xr_exec_context_vm_owner();
    if (!iso)
        return -1;
    XrClosure *closure = hook_instance_method(a, SYMBOL_OP_EQ);
    if (!closure)
        return -1;
    XrValue args[2] = {a, b};
    XrValue result = xr_vm_call_closure(iso, closure, args, 2);
    return XR_IS_BOOL(result) ? (XR_TO_BOOL(result) ? 1 : 0) : 0;
}

void xr_vm_install_value_hooks(void) {
    xr_value_set_instance_hooks(vm_instance_hash_hook, vm_instance_eq_hook);
}
