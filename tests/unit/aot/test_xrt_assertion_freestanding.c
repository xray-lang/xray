#include "aot/xrt_core_freestanding.h"

static char captured[256];
static size_t captured_length;
static unsigned writes;

_Noreturn void xr_hook_panic(const char *message, size_t length) {
    (void) message;
    (void) length;
    for (;;) {
    }
}

bool xr_hook_assertion_report(void *context, const char *bytes,
                              size_t length) {
    (void) context;
    if (!bytes || length >= sizeof(captured))
        return false;
    for (size_t i = 0; i < length; i++)
        captured[i] = bytes[i];
    captured[length] = '\0';
    captured_length = length;
    writes++;
    return true;
}

static bool bytes_equal(const char *left, const char *right) {
    if (!left || !right)
        return false;
    size_t i = 0;
    while (left[i] && right[i]) {
        if (left[i] != right[i])
            return false;
        i++;
    }
    return left[i] == right[i];
}

int main(void) {
    XrAssertionFailure failure = {
        .kind = XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL,
        .source = {"fixture.xr", 12, 4, 12, 15},
        .message = "numbers differ",
        .actual = "1",
        .expected = "2",
    };
    const char *expected = "AssertionFailure[values-not-equal] at fixture.xr:12:4\n"
                           "  message: numbers differ\n  actual: 1\n  expected: 2";
    char scratch[256];
    int required = xr_assertion_failure_render_size(&failure);
    if (required <= 0 || (size_t) required + 1u > sizeof(scratch))
        return 1;
    if (!xrt_freestanding_assertion_report(&failure, scratch,
                                           (size_t) required + 1u))
        return 2;
    if (writes != 1u || captured_length != (size_t) required ||
        !bytes_equal(captured, expected))
        return 3;

    writes = 0;
    if (xrt_freestanding_assertion_report(&failure, scratch,
                                          (size_t) required))
        return 4;
    if (writes != 0u || scratch[0] != '\0')
        return 5;

    failure.kind = XR_ASSERTION_FAILURE_COUNT;
    if (xrt_freestanding_assertion_report(&failure, scratch,
                                          sizeof(scratch)))
        return 6;
    if (!XR_IS_NULL(xrt_freestanding_assertion_condition(
            1, "fixture.xr", 1, 1, 1, 4, XR_NULL_VAL)))
        return 7;
    if (!XR_IS_NULL(xrt_freestanding_assertion_equal(
            XR_FROM_INT(7), XR_FROM_INT(7), "fixture.xr", 1, 1, 1, 4,
            XR_NULL_VAL)))
        return 8;
    bool equal = false;
    if (!xrt_freestanding_assertion_values_equal(XR_FROM_INT(1),
                                                 XR_FROM_INT(2), &equal) ||
        equal)
        return 9;
    if (xrt_freestanding_assertion_values_equal(XR_FROM_FLOAT(1.0),
                                                XR_FROM_FLOAT(1.0), &equal))
        return 10;
    return 0;
}
