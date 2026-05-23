/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_module.inc.c — module system dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, pc,
 * ci, frame, base, R, K, vmcase, vmbreak, VM_RUNTIME_ERROR,
 * VM_STACK, VM_STACK_CHECK, VM_SET_STACK_TOP, ...) provided by
 * the surrounding scope. CMake excludes *.inc.c from the
 * VM_SRC glob.
 *
 * Owns: OP_IMPORT, OP_LOAD_MODULE_SLOT, OP_LOAD_MODULE, OP_SET_EXPORT.
 *
 * Graph-resolved user modules use OP_LOAD_MODULE_SLOT / OP_LOAD_MODULE.
 * Stdlib and native modules use OP_IMPORT (runtime string-based lookup).
 * OP_SET_EXPORT writes to slot-indexed export arrays with symbol registration.
 */

vmcase(OP_IMPORT) {
    /* R[A] = import(K[Bx]) — runtime module import for stdlib/native.
     * Used when the target module is not in the compile-time graph. */
    int reg = GETARG_A(i);
    int bx = GETARG_Bx(i);
    XrValue module_name_val = K(bx);

    if (!XR_IS_STRING(module_name_val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "import: module name must be a string");
    }

    VM_STACK_CHECK(reg + 1);
    frame = ci;
    VM_SET_STACK_TOP(base + frame->closure->proto->maxstacksize);

    XrString *module_name = XR_TO_STRING(module_name_val);
    XrValue module_val = xr_module_import(isolate, module_name->data);

    base = VM_STACK + frame->base_offset;

    if (XR_IS_NULL(module_val)) {
        VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED, "import: failed to load module '%s'",
                         module_name->data);
    }

    R(reg) = module_val;
    vmbreak;
}

vmcase(OP_LOAD_MODULE_SLOT) {
    /* R[A] = modules[B].exports[C] — selective cross-module import.
     * B = topo index, C = export dense index within that module. */
    int dest = GETARG_A(i);
    int mod_idx = GETARG_B(i);
    int slot = GETARG_C(i);

    XrModuleRegistry *mreg = isolate->module_registry;
    if (!mreg || !mreg->module_table || mod_idx >= mreg->module_table_count) {
        VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED,
                         "LOAD_MODULE_SLOT: module_table not ready (mod=%d, slot=%d)", mod_idx,
                         slot);
    }
    XrModule *target = mreg->module_table[mod_idx];
    if (!target || slot >= target->export_count) {
        VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED,
                         "LOAD_MODULE_SLOT: bad module or slot (mod=%d, slot=%d)", mod_idx, slot);
    }
    R(dest) = target->export_values[slot];
    vmbreak;
}

vmcase(OP_LOAD_MODULE) {
    /* R[A] = modules[B] — whole-module import (namespace).
     * B = topo index in module_table. */
    int dest = GETARG_A(i);
    int mod_idx = GETARG_B(i);

    XrModuleRegistry *mreg = isolate->module_registry;
    if (!mreg || !mreg->module_table || mod_idx >= mreg->module_table_count) {
        VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED, "LOAD_MODULE: module_table not ready (mod=%d)",
                         mod_idx);
    }
    XrModule *target = mreg->module_table[mod_idx];
    if (!target) {
        VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED, "LOAD_MODULE: NULL module at index %d", mod_idx);
    }
    R(dest) = xr_value_from_module(target);
    vmbreak;
}

vmcase(OP_SET_EXPORT) {
    /* module.exports[A] = R[B], name = K[C].
     * Slot-indexed write with symbol registration.
     * Auto-grows export arrays when slot >= current capacity. */
    int slot = GETARG_A(i);
    int src_reg = GETARG_B(i);
    int name_idx = GETARG_C(i);

    XrModule *cur = isolate->current_module;
    if (!cur) {
        /* No current module — entry scripts run without a module context.
         * Silently skip the export (matches standalone script behavior). */
        vmbreak;
    }

    /* Ensure export arrays are large enough for this slot */
    uint16_t needed = (uint16_t) (slot + 1);
    if (needed > cur->export_capacity) {
        uint16_t new_cap = needed < 8 ? 8 : (uint16_t) (needed * 2);
        XR_REALLOC_OR_ABORT(cur->export_values, (size_t) new_cap * sizeof(XrValue),
                            "SET_EXPORT grow values");
        XR_REALLOC_OR_ABORT(cur->export_symbols, (size_t) new_cap * sizeof(SymbolId),
                            "SET_EXPORT grow symbols");
        XR_REALLOC_OR_ABORT(cur->export_flags, (size_t) new_cap * sizeof(uint8_t),
                            "SET_EXPORT grow flags");
        for (uint16_t j = cur->export_capacity; j < new_cap; j++) {
            cur->export_values[j] = xr_null();
            cur->export_symbols[j] = -1;
            cur->export_flags[j] = 0;
        }
        cur->export_capacity = new_cap;
    }
    if (needed > cur->export_count)
        cur->export_count = needed;

    cur->export_values[slot] = R(src_reg);

    /* Register the export symbol name so xr_module_get_sym works */
    XrValue name_val = K(name_idx);
    if (XR_IS_STRING(name_val)) {
        XrString *name_str = XR_TO_STRING(name_val);
        XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
        SymbolId sym = xr_symbol_register_in_table(sym_table, name_str->data);
        cur->export_symbols[slot] = sym;
    }
    vmbreak;
}
