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
 * Owns:
 *   - OP_ASSERT      : assert / assert_true / assert_false
 *   - OP_ASSERT_EQ   : assert_eq (deep equality)
 *   - OP_ASSERT_NE   : assert_ne (deep inequality)
 *   - OP_REGEX_COMPILE: regex literal — bridges to stdlib/regex
 */

/* === Assertion instructions (test framework) === */

vmcase(OP_ASSERT) {
    /* if !R[A] then throw AssertError(K[B])
     * A = condition register
     * B = location info constant index (string)
     * C = 0: assert/assert_true, 1: assert_false (negate)
     */
    int cond_reg = GETARG_A(i);
    int loc_idx = GETARG_B(i);
    int negate = GETARG_C(i);

    XrValue cond = R(cond_reg);
    bool truthy = xr_vm_is_truthy(cond);

    // C=1: assert_false — fail if truthy
    bool failed = negate ? truthy : !truthy;

    if (failed) {
        XrValue loc_val = K(loc_idx);
        const char *loc_str = XR_IS_STRING(loc_val) ? XR_TO_STRING(loc_val)->data : "unknown";
        const char *fn_name = negate ? "assert_false" : "assert";

        if (!isolate->suppress_exception_print) {
            fprintf(stderr, "\n");
            fprintf(stderr, "ASSERTION FAILED at %s\n", loc_str);
            fprintf(stderr, "  %s() condition is %s\n", fn_name, negate ? "true" : "false");
            fprintf(stderr, "\n");
        }
        VM_RUNTIME_ERROR(0, "assertion failed at %s", loc_str);
    }
    vmbreak;
}

vmcase(OP_ASSERT_EQ) {
    /* if R[A] != R[B] then throw AssertError(K[C])
     * A = actual value register
     * B = expected value register
     * C = location info constant index
     */
    int actual_reg = GETARG_A(i);
    int expect_reg = GETARG_B(i);
    int loc_idx = GETARG_C(i);

    XrValue actual = R(actual_reg);
    XrValue expect = R(expect_reg);

    if (!xr_value_deep_eq(actual, expect)) {
        XrValue loc_val = K(loc_idx);
        const char *loc_str = XR_IS_STRING(loc_val) ? XR_TO_STRING(loc_val)->data : "unknown";
        if (!isolate->suppress_exception_print) {
            fprintf(stderr, "\n");
            fprintf(stderr, "ASSERTION FAILED at %s\n", loc_str);
            fprintf(stderr, "  assert_eq() values are not equal\n");
            fprintf(stderr, "    actual:   %s\n", xr_value_to_string(isolate, actual)->data);
            fprintf(stderr, "    expected: %s\n\n", xr_value_to_string(isolate, expect)->data);
        }
        VM_RUNTIME_ERROR(0, "assertion failed at %s: values not equal", loc_str);
    }
    vmbreak;
}

vmcase(OP_ASSERT_NE) {
    /* if R[A] == R[B] then throw AssertError(K[C])
     * A = actual value register
     * B = unexpected value register
     * C = location info constant index
     */
    int actual_reg = GETARG_A(i);
    int unexpected_reg = GETARG_B(i);
    int loc_idx = GETARG_C(i);

    XrValue actual = R(actual_reg);
    XrValue unexpected = R(unexpected_reg);

    if (xr_value_deep_eq(actual, unexpected)) {
        XrValue loc_val = K(loc_idx);
        const char *loc_str = XR_IS_STRING(loc_val) ? XR_TO_STRING(loc_val)->data : "unknown";

        if (!isolate->suppress_exception_print) {
            fprintf(stderr, "\n");
            fprintf(stderr, "ASSERTION FAILED at %s\n", loc_str);
            fprintf(stderr, "  assert_ne() values should not be equal\n");
            fprintf(stderr, "    value: %s\n\n", xr_value_to_string(isolate, actual)->data);
        }
        VM_RUNTIME_ERROR(0, "assertion failed at %s: values should not be equal", loc_str);
    }
    vmbreak;
}

/* === Regex Literal === */

vmcase(OP_REGEX_COMPILE) {
    /* R[A] = regex.compile(K[B], K[C]) — pattern and flags
     * are guaranteed string constants by the compiler.
     * The actual flag parse + xr_regex_compile() lives
     * behind xr_regex_compile_literal() in stdlib/regex
     * to keep src/vm free of stdlib reverse includes. */
    int a = GETARG_A(i);
    R(a) = xr_regex_compile_literal(isolate, K(GETARG_B(i)), K(GETARG_C(i)));
    vmbreak;
}
