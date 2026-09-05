/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_assert.inc.c — assertion and regex-literal dispatch
 *
 * NOT a standalone translation unit. This file is `#include`d from
 * inside the dispatch switch in xvm.c. It deliberately uses the
 * locals (i, isolate, pc, base, R, K, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, ...) defined by the surrounding scope. Compiling
 * it on its own will fail; CMake explicitly excludes *.inc.c from
 * the VM_SRC glob.
 *
 * Owns the typed assertion bytecode record.
 */

/* === Assertion instructions (test framework) === */

vmcase(OP_ASSERTION) {
    XR_ASSERTION_OWNER_GUARD(XR_SEM_OWNER_ID_SHARED_ASSERTION_HI,
                             XR_SEM_OWNER_ID_SHARED_ASSERTION_LO);
    XR_ASSERTION_CONSUMER_GUARD(XR_SEM_CONSUMER_VM);
    /* The primary instruction is followed by three typed metadata rows.  They
     * are one indivisible bytecode record; orphan marker execution and every
     * malformed field fail closed instead of falling back to a source name. */
    XrInstruction *_code_end = PROTO_CODE_BASE(cl->proto) + (ptrdiff_t) PROTO_CODE_COUNT(cl->proto);
    if (pc + 3 > _code_end || GET_OPCODE(pc[0]) != OP_ASSERTION_FILE ||
        GET_OPCODE(pc[1]) != OP_ASSERTION_SPAN_START || GET_OPCODE(pc[2]) != OP_ASSERTION_SPAN_END)
        VM_RUNTIME_ERROR(0, "malformed typed assertion bytecode record");

    XrInstruction _file_inst = pc[0];
    XrInstruction _start_inst = pc[1];
    XrInstruction _end_inst = pc[2];
    pc += 3;

    uint32_t _base_reg = GETARG_A(i);
    XrAssertionFailureKind _failure_kind = (XrAssertionFailureKind) GETARG_B(i);
    XrAssertionKind _plan_kind = (XrAssertionKind) GETARG_C(i);
    uint32_t _file_index = GETARG_Bx(_file_inst);
    uint64_t _start = GETARG_Ax(_start_inst);
    uint64_t _end = GETARG_Ax(_end_inst);
    if (_base_reg > MAXARG_A - 2u || _base_reg + 2u >= cl->proto->maxstacksize ||
        GETARG_A(_file_inst) != XR_ASSERTION_PLAN_SCHEMA_VERSION ||
        _file_index >= PROTO_CONST_COUNT(cl->proto) || _failure_kind <= XR_ASSERTION_FAILURE_NONE ||
        _failure_kind >= XR_ASSERTION_FAILURE_COUNT || _plan_kind <= XR_ASSERTION_KIND_NONE ||
        _plan_kind >= XR_ASSERTION_KIND_COUNT)
        VM_RUNTIME_ERROR(0, "invalid typed assertion bytecode metadata");

    XrValue _file_value = K(_file_index);
    if (!XR_IS_STRING(_file_value))
        VM_RUNTIME_ERROR(0, "typed assertion source file is not a string constant");
    XrLocation _source = {
        .file = XR_TO_STRING(_file_value)->data,
        .line = (uint32_t) (_start >> 24u),
        .column = (uint32_t) (_start & UINT64_C(0xFFFFFF)),
        .end_line = (uint32_t) (_end >> 24u),
        .end_column = (uint32_t) (_end & UINT64_C(0xFFFFFF)),
    };
    if (!xr_location_is_complete(_source))
        VM_RUNTIME_ERROR(0, "typed assertion bytecode has an invalid source span");

    bool _failed = true;
    if (_plan_kind == XR_ASSERTION_KIND_CONDITION) {
        if (_failure_kind != XR_ASSERTION_FAILURE_CONDITION_FALSE || !XR_IS_BOOL(R(_base_reg)))
            VM_RUNTIME_ERROR(0, "condition assertion bytecode violates its typed plan");
        _failed = !XR_TO_BOOL(R(_base_reg));
    } else if (_plan_kind == XR_ASSERTION_KIND_EQUAL) {
        if (_failure_kind != XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL)
            VM_RUNTIME_ERROR(0, "equality assertion bytecode violates its typed plan");
        _failed = !xr_value_deep_eq(R(_base_reg), R(_base_reg + 1u));
    } else if (_plan_kind == XR_ASSERTION_KIND_THROWS) {
        if (_failure_kind != XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR &&
            _failure_kind != XR_ASSERTION_FAILURE_UNEXPECTED_PANIC &&
            _failure_kind != XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS)
            VM_RUNTIME_ERROR(0, "typed-error assertion bytecode has an impossible failure kind");
    } else if (_failure_kind != XR_ASSERTION_FAILURE_EXPECTED_PANIC &&
               _failure_kind != XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR &&
               _failure_kind != XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS) {
        VM_RUNTIME_ERROR(0, "panic assertion bytecode has an impossible failure kind");
    }
    if (!_failed)
        vmbreak;

    XrCoroutine *_assertion_coro = (XrCoroutine *) VM_CURRENT_CORO;
    XrCoroHeap *_assertion_heap = _assertion_coro ? _assertion_coro->heap : vm_exec_local_heap();
#define XR_VM_ASSERTION_RELEASE(value)                                                             \
    do {                                                                                           \
        if (XR_IS_PTR(value))                                                                      \
            xr_rc_release_value(_assertion_heap, (value));                                         \
        (value) = XR_NULL_VAL;                                                                     \
    } while (0)
#define XR_VM_ASSERTION_RELEASE_ACTION_OBSERVATIONS()                                              \
    do {                                                                                           \
        if (_plan_kind == XR_ASSERTION_KIND_THROWS || _plan_kind == XR_ASSERTION_KIND_PANICS) {    \
            XR_VM_ASSERTION_RELEASE(R(_base_reg));                                                 \
            XR_VM_ASSERTION_RELEASE(R(_base_reg + 1u));                                            \
        }                                                                                          \
    } while (0)

    const char *_message = NULL;
    if (!XR_IS_NULL(R(_base_reg + 2u))) {
        if (!XR_IS_STRING(R(_base_reg + 2u))) {
            XR_VM_ASSERTION_RELEASE_ACTION_OBSERVATIONS();
            VM_RUNTIME_ERROR(0, "typed assertion message is not a string");
        }
        _message = XR_TO_STRING(R(_base_reg + 2u))->data;
    }
    XrString *_actual_text = NULL;
    XrString *_expected_text = NULL;
    XrString *_caught_error_text = NULL;
    XrString *_caught_panic_text = NULL;
    bool _actual_text_owned = false;
    bool _expected_text_owned = false;
    bool _caught_error_text_owned = false;
    bool _caught_panic_text_owned = false;
    if (_failure_kind == XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL) {
        _actual_text_owned = !XR_IS_STRING(R(_base_reg));
        _expected_text_owned = !XR_IS_STRING(R(_base_reg + 1u));
        _actual_text = xr_value_to_string(isolate, R(_base_reg));
        _expected_text = xr_value_to_string(isolate, R(_base_reg + 1u));
    } else if (_failure_kind == XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR) {
        _caught_error_text_owned = !XR_IS_STRING(R(_base_reg));
        _caught_error_text = xr_value_to_string(isolate, R(_base_reg));
    } else if (_failure_kind == XR_ASSERTION_FAILURE_UNEXPECTED_PANIC) {
        _caught_panic_text_owned = !XR_IS_STRING(R(_base_reg + 1u));
        _caught_panic_text = xr_value_to_string(isolate, R(_base_reg + 1u));
    } else if (_failure_kind == XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS) {
        _caught_error_text_owned = !XR_IS_STRING(R(_base_reg));
        _caught_panic_text_owned = !XR_IS_STRING(R(_base_reg + 1u));
        _caught_error_text = xr_value_to_string(isolate, R(_base_reg));
        _caught_panic_text = xr_value_to_string(isolate, R(_base_reg + 1u));
    }
    if ((_actual_text_owned && !_actual_text) || (_expected_text_owned && !_expected_text) ||
        (_caught_error_text_owned && !_caught_error_text) ||
        (_caught_panic_text_owned && !_caught_panic_text)) {
        if (_actual_text_owned && _actual_text)
            xr_rc_release_value(_assertion_heap, xr_string_value(_actual_text));
        if (_expected_text_owned && _expected_text)
            xr_rc_release_value(_assertion_heap, xr_string_value(_expected_text));
        if (_caught_error_text_owned && _caught_error_text)
            xr_rc_release_value(_assertion_heap, xr_string_value(_caught_error_text));
        if (_caught_panic_text_owned && _caught_panic_text)
            xr_rc_release_value(_assertion_heap, xr_string_value(_caught_panic_text));
        XR_VM_ASSERTION_RELEASE_ACTION_OBSERVATIONS();
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "typed assertion value formatter failed");
    }
    XrAssertionFailure _failure = {
        .kind = _failure_kind,
        .source = _source,
        .message = _message,
        .actual = _actual_text ? _actual_text->data : NULL,
        .expected = _expected_text ? _expected_text->data : NULL,
        .caught_error = _caught_error_text ? _caught_error_text->data : NULL,
        .caught_panic = _caught_panic_text ? _caught_panic_text->data : NULL,
    };
    int _rendered_size = xr_assertion_failure_render_size(&_failure);
    char *_rendered = _rendered_size >= 0 ? (char *) xr_malloc((size_t) _rendered_size + 1u) : NULL;
    const char *_render_error = NULL;
    int _render_error_code = 0;
    if (_rendered_size < 0) {
        _render_error = "typed assertion failure schema is invalid";
    } else if (!_rendered) {
        _render_error = "failed to render assertion failure";
        _render_error_code = XR_ERR_OUT_OF_MEMORY;
    } else if (xr_assertion_failure_render(_rendered, (size_t) _rendered_size + 1u, &_failure) !=
               _rendered_size) {
        _render_error = "typed assertion renderer rejected exact capacity";
    }
    if (_actual_text_owned && _actual_text)
        xr_rc_release_value(_assertion_heap, xr_string_value(_actual_text));
    if (_expected_text_owned && _expected_text)
        xr_rc_release_value(_assertion_heap, xr_string_value(_expected_text));
    if (_caught_error_text_owned && _caught_error_text)
        xr_rc_release_value(_assertion_heap, xr_string_value(_caught_error_text));
    if (_caught_panic_text_owned && _caught_panic_text)
        xr_rc_release_value(_assertion_heap, xr_string_value(_caught_panic_text));

    /* Action observations and a normal action result are owned by this typed
     * boundary.  Equality/condition operands remain borrowed SSA values. */
    XR_VM_ASSERTION_RELEASE_ACTION_OBSERVATIONS();
    if (_render_error) {
        xr_free(_rendered);
        VM_RUNTIME_ERROR(_render_error_code, "%s", _render_error);
    }
    XrValue _assertion_exception = xr_panic_info_newf(isolate, 0, "%s", _rendered);
    xr_free(_rendered);
#undef XR_VM_ASSERTION_RELEASE_ACTION_OBSERVATIONS
#undef XR_VM_ASSERTION_RELEASE
    VM_THROW_EXCEPTION_VALUE(_assertion_exception);
}

vmcase(OP_ASSERTION_FILE) {
    VM_RUNTIME_ERROR(0, "orphan typed assertion file metadata");
}

vmcase(OP_ASSERTION_SPAN_START) {
    VM_RUNTIME_ERROR(0, "orphan typed assertion start-span metadata");
}

vmcase(OP_ASSERTION_SPAN_END) {
    VM_RUNTIME_ERROR(0, "orphan typed assertion end-span metadata");
}
