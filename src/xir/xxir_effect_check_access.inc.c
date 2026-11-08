/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_check_access.inc.c - Hidden storage access seeds on numeric equations
 */
/* Only explicit value/local/Cell operations have no hidden module source.
 * Native, child execution and any new opcode need their own access facts. */
static bool effect_default_access_local(XrXirOp op) {
    switch (op) {
    case XR_XIR_TASK_CANCEL: case XR_XIR_TASK_POLL:
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_FLOAT:
    case XR_XIR_CONST_STRING: case XR_XIR_CONST_RUNE: case XR_XIR_COPY:
    case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_CONCAT_STRING:
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT: case XR_XIR_DIV_INT:
    case XR_XIR_REM_INT: case XR_XIR_EQ_INT: case XR_XIR_NE_INT: case XR_XIR_LT_INT:
    case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT: case XR_XIR_AND_INT:
    case XR_XIR_OR_INT: case XR_XIR_XOR_INT: case XR_XIR_SHL_INT: case XR_XIR_SHR_INT:
    case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT: case XR_XIR_LT_FLOAT:
    case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT: case XR_XIR_CONVERT_NUMBER:
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT:
    case XR_XIR_CLEANUP_REGISTER: case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR:
    case XR_XIR_INVOKE_RESULT: case XR_XIR_INVOKE_ERROR: case XR_XIR_INVOKE_DISCARD:
    case XR_XIR_JUMP: case XR_XIR_BRANCH: case XR_XIR_RETURN: case XR_XIR_THROW:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_LOCAL_WRITE: case XR_XIR_LOCAL_UNINIT:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_WRITE:
    case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_PHI: case XR_XIR_FUNCTION_REF: case XR_XIR_CLASS_METHOD_REF: case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_CELL_TAKE: case XR_XIR_VALUE_TAKE: case XR_XIR_CELL_NEW_TAKE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_CELL_PLACE: case XR_XIR_CELL_PROJECT: case XR_XIR_OBJECT_PLACE:
    case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET: case XR_XIR_ARRAY_PUSH:
    case XR_XIR_ARRAY_LEN: case XR_XIR_ARRAY_REPEAT: case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_BYTE_ARRAY_APPEND: case XR_XIR_BYTE_ARRAY_REPEAT: case XR_XIR_ARRAY_RESERVE: case XR_XIR_CELL_STRING_APPEND:
    case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING: case XR_XIR_NE_STRING: case XR_XIR_LT_STRING:
    case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_RUNE_IS_WHITESPACE: case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE:
    case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET: case XR_XIR_STRUCT_SET:
    case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG: case XR_XIR_ENUM_GET:
    case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET: case XR_XIR_CLASS_SET:
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME: case XR_XIR_NULLABLE_UNWRAP:
    case XR_XIR_FIXED_ARRAY_NEW: case XR_XIR_FIXED_ARRAY_GET: case XR_XIR_FIXED_ARRAY_LEN:
    case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD:
    case XR_XIR_VIEW_NEW: case XR_XIR_VIEW_REBORROW: case XR_XIR_VIEW_LEN: case XR_XIR_VIEW_GET:
    case XR_XIR_VIEW_SET: case XR_XIR_VIEW_COPY: case XR_XIR_VIEW_END: case XR_XIR_VIEW_CLEANUP:
    case XR_XIR_ERROR_ERASE: case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW:
    case XR_XIR_PANIC_ERROR: case XR_XIR_MATCH_FAIL: case XR_XIR_ASSERT_CONDITION: case XR_XIR_PANIC_CATCH:
    case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE: case XR_XIR_PANIC_REPLACED_ERROR:
        return true;
    default: return false;
    }
}
static bool effect_default_access_scalar_slot(const XrXirModule *module,uint32_t function,
    const XrXirInstruction *op) {
    const XrXirDeclarations *d=module->declarations;
    if(op->op!=XR_XIR_SLOT_LOAD || !d || !d->functions || !d->slots ||
        function>=module->function_count || op->immediate<0 || (uint64_t)op->immediate>=d->slot_count)
        return false;
    const XrXirSlot *slot=&d->slots[op->immediate];
    bool scalar=slot->type==XR_XIR_BOOL || slot->type==XR_XIR_RUNE || xr_xir_type_is_number(slot->type);
    /* Loading an immutable scalar has no referent that can overlap a loan.
     * Declaration, import, initialization and ordinary ROOT checks still apply. */
    return scalar && !slot->mutable && op->type==slot->type &&
        d->functions[function].module==slot->module &&
        xr_xir_module_imports(d,d->functions[function].module,slot->module);
}
