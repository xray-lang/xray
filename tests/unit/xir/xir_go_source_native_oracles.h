typedef struct SourceTaskOracle { const char *name; XrXirCallStatus status; unsigned kind; int64_t value; } SourceTaskOracle;
enum { TASK_ORACLE_I64, TASK_ORACLE_STRING, TASK_ORACLE_NULLABLE, TASK_ORACLE_ERROR, TASK_ORACLE_PANIC };
static const SourceTaskOracle source_task_oracles[] = {
    {"integer", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"string", XR_XIR_CALL_RETURNED, TASK_ORACLE_STRING, 0},
    {"generic", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"context_ordinary", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_go", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_existing", XR_XIR_CALL_RETURNED, TASK_ORACLE_NULLABLE, 42},
    {"context_spawn", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"grouped", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"import_default", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"const_state", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"local_storage", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 43},
    {"unknown_task", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 84},
    {"value_error", XR_XIR_CALL_THROWN, TASK_ORACLE_ERROR, 0},
    {"shadow_class", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"context_match", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_match_block", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_match_existing", XR_XIR_CALL_RETURNED, TASK_ORACLE_NULLABLE, 42}
};
static void source_task_assert(const SourceTaskOracle *oracle, const XrXirCallResult *outcome) {
    CHECK(outcome->status == oracle->status && xr_xir_call_result_valid(outcome));
    if (oracle->kind == TASK_ORACLE_I64) CHECK(outcome->value.type == XR_XIR_I64 && outcome->value.payload == oracle->value);
    else if (oracle->kind == TASK_ORACLE_STRING) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&outcome->value, &bytes, &length) && length == 6 && !memcmp(bytes, "A\0BA\0B", 6));
    } else if (oracle->kind == TASK_ORACLE_NULLABLE) {
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(&outcome->value, &some, &payload) && some && payload &&
            payload->type == XR_XIR_I64 && payload->payload == 42);
    } else if (oracle->kind == TASK_ORACLE_ERROR) {
        XrXirValue underlying = outcome->value;
        if (underlying.type == XR_XIR_ERROR) CHECK(xr_xir_error_borrow(&outcome->value, &underlying));
        XrXirEnumBorrow value = {0}; const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_enum_borrow(&underlying, &value) == XR_XIR_VALUE_OK && value.name.length == 7 &&
            !memcmp(value.name.bytes, "Problem", 7) && value.member.length == 6 &&
            !memcmp(value.member.bytes, "Failed", 6) && value.field_count == 1);
        CHECK(xr_xir_string_view(&value.fields[0], &bytes, &length) && length == 5 && !memcmp(bytes, "owned", 5));
    } else {
        CHECK(!outcome->value.type && !outcome->value.reserved && !outcome->value.payload);
        CHECK(outcome->panic.detail.code == XR_XIR_PANIC_NULL_UNWRAP && !outcome->panic.detail.reserved &&
            !outcome->panic.detail.index && !outcome->panic.detail.length);
    }
}
