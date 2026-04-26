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
 * Owns: OP_IMPORT, OP_EXPORT, OP_EXPORT_ALL.
 */

vmcase(OP_IMPORT) {
    // R[A] = import(K[Bx]) - Import module
    int reg = GETARG_A(i);
    int bx = GETARG_Bx(i);
    XrValue module_name_val = K(bx);

    if (!XR_IS_STRING(module_name_val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "import: module name must be a string");
    }

    // Ensure stack has space for result register
    VM_STACK_CHECK(reg + 1);
    // After stack check, base/ci/frame may have changed due to realloc
    frame = ci;

    /*
     * Update stack_top to point to current frame's stack top
     * This ensures module execution uses stack space that doesn't overlap with current frame's local variables
     */
    VM_SET_STACK_TOP(base + frame->closure->proto->maxstacksize);

    /*
     * SAFETY NOTE: module_name_val is from constant pool (K[bx]), not from stack.
     * Stack reallocation (VM_STACK_CHECK) only affects stack pointers, not proto->constants.
     * The XrString pointer remains valid across xr_module_import call.
     */
    XrString *module_name = XR_TO_STRING(module_name_val);
    XrValue module_val = xr_module_import(isolate, module_name->data);

    /*
     * Module import may cause stack reallocation (nested imports).
     * Must refresh base pointer after import.
     */
    base = VM_STACK + frame->base_offset;

    if (XR_IS_NULL(module_val)) {
        VM_RUNTIME_ERROR(XR_ERR_MOD_LOAD_FAILED, "import: failed to load module '%s'", module_name->data);
    }

    R(reg) = module_val;
    vmbreak;
}

vmcase(OP_EXPORT) {
    // export(K[A], R[B], C) - Export value to current module, C=1 means constant
    int name_idx = GETARG_A(i);
    int value_reg = GETARG_B(i);
    int is_const = GETARG_C(i);

    XrValue name_val = K(name_idx);
    if (!XR_IS_STRING(name_val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "export: name must be a string");
    }

    XrString *name = XR_TO_STRING(name_val);
    XrValue value = R(value_reg);

    // Add to current module export table (pass const flag)
    xr_module_add_current_export(isolate, name->data, value, is_const != 0);
    vmbreak;
}

vmcase(OP_EXPORT_ALL) {
    // export * from R[A] - Export all members from module to current module
    int module_reg = GETARG_A(i);
    XrValue module_val = R(module_reg);

    XrModule *src_module = xr_value_to_module(module_val);
    if (!src_module || src_module->export_count == 0) {
        vmbreak;
    }

    // Iterate source module's flat export arrays
    XrModule *dst_module = isolate->current_module;
    if (dst_module) {
        for (uint16_t idx = 0; idx < src_module->export_count; idx++) {
            xr_module_add_export_sym(isolate, dst_module,
                src_module->export_symbols[idx],
                src_module->export_values[idx], false);
        }
    }
    vmbreak;
}
