#include "xir/xxir_program.h"
#include "xir/xxir_float.h"
#include "xir/xxir_instance_value.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_class.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include "xir/xxir_panic.h"
#include "xir/xxir_equal.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#if !defined(XR_ARCH_X86_64)
#error XIR_target_mismatch
#endif
_Static_assert(XR_XIR_CALL_ABI_VERSION == 25u, "XIR call ABI");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 20u, "XIR scalar ABI");
_Static_assert(sizeof(XrXirValue) == 16, "XIR scalar size");
_Static_assert(_Alignof(XrXirValue) == 8, "XIR scalar alignment");
_Static_assert(offsetof(XrXirValue, payload) == 8, "XIR payload offset");
_Static_assert(sizeof(XrXirFaultDetail) == 24 && _Alignof(XrXirFaultDetail) == 8, "XIR fault layout");
_Static_assert(offsetof(XrXirFaultDetail, index) == 8 && offsetof(XrXirFaultDetail, length) == 16, "XIR fault offsets");
_Static_assert(sizeof(XrXirPanicPayload) == 40 && _Alignof(XrXirPanicPayload) == 8, "XIR panic layout");
_Static_assert(offsetof(XrXirPanicPayload, detail) == 0 && offsetof(XrXirPanicPayload, message) == 24, "XIR panic offsets");
_Static_assert(sizeof(XrXirAction) == 88 && _Alignof(XrXirAction) == 8, "XIR action layout");
_Static_assert(offsetof(XrXirAction, value) == 24 && offsetof(XrXirAction, panic) == 40 && offsetof(XrXirAction, flags) == 80, "XIR action offsets");
_Static_assert(sizeof(XrXirCallResult) == 72 && _Alignof(XrXirCallResult) == 8, "XIR result layout");
_Static_assert(offsetof(XrXirCallResult, value) == 8 && offsetof(XrXirCallResult, wake) == 24 && offsetof(XrXirCallResult, panic) == 32, "XIR result offsets");
static XR_NOINLINE bool old28_accept_inbox(XrXirCallView *view, unsigned char *frame,
    uint32_t *pc, uint32_t *destination, uint32_t *expected, uint32_t normal_pc,
    uint32_t error_pc, uint32_t error_destination, bool *invoking, bool discard,
    XrXirAction *terminal) {
    XrXirValue inbox = view->inbox.value;
    bool discarded = false;
    if (*invoking) {
        bool error = view->inbox.status == XR_XIR_CALL_THROWN;
        if (!error && view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;
        *pc = error ? error_pc : normal_pc;
        *invoking = false;
        if (error) {
            *expected = XR_XIR_ERROR;
            *destination = error_destination;
            inbox.type = XR_XIR_ERROR;
        } else if (discard) {
            if (xr_xir_call_discard_inbox(view, (XrXirType)*expected) != XR_XIR_CALL_READY)
                goto invalid;
            discarded = true;
        }
    } else if (view->inbox.status == XR_XIR_CALL_THROWN) {
        *terminal = (XrXirAction){XR_XIR_ACTION_THROW, 0, NULL, 0, inbox, {0}, 0};
        return true;
    }
    if (!discarded) {
        if (view->inbox.status != XR_XIR_CALL_RETURNED && view->inbox.status != XR_XIR_CALL_THROWN)
            goto invalid;
        if (*expected == XR_XIR_UNIT) {
            if (inbox.type || inbox.reserved || inbox.payload) goto invalid;
        } else if (!xr_xir_value_argument(&inbox, view->arena, (XrXirType)*expected))
            goto invalid;
        if (xr_xir_type_is_owned(xr_xir_compile_type_arena_types(view->arena), (XrXirType)*expected)) {
            if (xr_xir_owned_slot_copy(frame, *destination, view->arena,
                    (XrXirType)*expected, inbox.payload) != XR_XIR_VALUE_OK) goto limit;
        } else if (*destination != UINT32_MAX) {
            xr_xir_scalar_store(frame, *destination, inbox.payload);
        }
    }
    return false;
invalid:
    *terminal = (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
    return true;
limit:
    *terminal = (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
        {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
    return true;
}
typedef struct old28_state_0 {
    uint32_t pc, destination, expected, normal_pc, error_pc, error_destination, panic_pc, panic_destination;
    bool initialized, waiting, invoking, discard;
    unsigned char frame[1];
} old28_state_0;
static XR_NOINLINE bool old28_accept_waiting(XrXirCallView *view, unsigned char *frame,
    bool *waiting, uint32_t *pc, uint32_t *panic_pc, uint32_t panic_destination,
    uint32_t *destination, uint32_t *expected, uint32_t normal_pc, uint32_t error_pc,
    uint32_t error_destination, bool *invoking, bool discard, XrXirAction *terminal) {
    *waiting = false;
    uint32_t handler_pc = *panic_pc; *panic_pc = 0;
    if (xr_xir_call_panic_status(view->inbox.status)) {
        if (!handler_pc) {
            *terminal = (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
            return true;
        }
        *invoking = false;
        *terminal = xr_xir_instance_panic_land(view, frame,
            (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
                {XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, 0},
            panic_destination, handler_pc, pc);
        return true;
    }
    return old28_accept_inbox(view, frame, pc, destination, expected, normal_pc,
        error_pc, error_destination, invoking, discard, terminal);
}
XR_FUNC XrXirAction old28_f0(XrXirCallView *view) {
    old28_state_0 *state = view->state;
    if (view->phase == XR_XIR_CALL_EXIT)
        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};
    if (!state->initialized) {
        if (view->argument_count != 0u) goto invalid;
        state->initialized = true;
    }
    if (state->waiting) {
        XrXirAction waiting_action = {0};
        if (old28_accept_waiting(view, state->frame, &state->waiting,
                &state->pc, &state->panic_pc, state->panic_destination, &state->destination,
                &state->expected, state->normal_pc, state->error_pc, state->error_destination,
                &state->invoking, state->discard, &waiting_action)) return waiting_action;
    }
    switch (state->pc) {
    case 0u:
        state->pc = 1u;
        return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {0u, 0, 0}, {0}, 0};
    default: break;
    }
invalid:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
}
static void old28_release_0(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason; (void) view;
}
typedef struct old28_state_1 {
    uint32_t pc, destination, expected, normal_pc, error_pc, error_destination, panic_pc, panic_destination;
    bool initialized, waiting, invoking, discard;
    unsigned char frame[8];
} old28_state_1;
XR_FUNC XrXirAction old28_f1(XrXirCallView *view) {
    old28_state_1 *state = view->state;
    if (view->phase == XR_XIR_CALL_EXIT)
        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};
    if (!state->initialized) {
        if (view->argument_count != 0u) goto invalid;
        state->initialized = true;
    }
    if (state->waiting) {
        XrXirAction waiting_action = {0};
        if (old28_accept_waiting(view, state->frame, &state->waiting,
                &state->pc, &state->panic_pc, state->panic_destination, &state->destination,
                &state->expected, state->normal_pc, state->error_pc, state->error_destination,
                &state->invoking, state->discard, &waiting_action)) return waiting_action;
    }
    switch (state->pc) {
    case 0u:
        state->pc = 1u;
        xr_xir_scalar_store(state->frame, 0u, INT64_C(42));
        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};
    case 1u:
        state->pc = 2u;
        return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {2u, 0, xr_xir_scalar_load(state->frame, 0u)}, {0}, 0};
    default: break;
    }
invalid:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
}
static void old28_release_1(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason; (void) view;
}
XR_DATADEF const XrXirCallEntry old28_entries[] = {
    {XR_XIR_CALL_ABI_VERSION, NULL, 0u, (XrXirType) 0, (uint32_t) sizeof(old28_state_0), old28_f0, old28_release_0, NULL, 0u, 0u},
    {XR_XIR_CALL_ABI_VERSION, NULL, 0u, (XrXirType) 2, (uint32_t) sizeof(old28_state_1), old28_f1, old28_release_1, NULL, 0u, 0u},
};
static const XrXirNominalIdentity old28_nominal_identities[] = {
    {{(const char *)(const unsigned char[]){
    0x72,0x6f,0x6f,0x74,
    0}, 4u}, {(const char *)(const unsigned char[]){
    0x46,0x69,0x72,0x73,0x74,
    0}, 5u}, 1u, 0u, NULL, 0u, 0u, NULL, 0u, 0u},
    {{(const char *)(const unsigned char[]){
    0x72,0x6f,0x6f,0x74,
    0}, 4u}, {(const char *)(const unsigned char[]){
    0x53,0x65,0x63,0x6f,0x6e,0x64,
    0}, 6u}, 1u, 0u, NULL, 0u, 0u, NULL, 0u, 0u},
};
static const XrXirNominalTable old28_nominals = {NULL, 2u, old28_nominal_identities};
static const XrXirTypes old28_types = {NULL, 0u, &old28_nominals, NULL};
static const uint8_t old28_checked[] = {
    88,82,67,72,75,0,0,0,24,0,0,0,63,0,0,0,
    2,0,0,0,0,0,0,0,183,1,0,0,0,0,0,0,
    225,85,220,7,110,106,76,243,161,150,188,104,1,104,180,43,
    100,234,181,181,19,10,59,139,46,21,89,243,36,110,58,163,
    0,0,0,0,2,0,0,0,1,0,0,0,4,0,0,0,
    105,110,105,116,0,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,
    1,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    4,0,0,0,109,97,105,110,0,0,0,0,2,0,0,0,
    1,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,
    0,0,0,0,2,0,0,0,2,0,0,0,2,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    42,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
    4,0,0,0,114,111,111,116,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,2,0,0,0,0,0,0,0,4,0,0,0,
    114,111,111,116,5,0,0,0,70,105,114,115,116,1,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,4,0,0,0,114,111,111,116,6,0,0,
    0,83,101,99,111,110,100,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,
};
static const uint8_t old28_identity[32] = {45,181,41,112,4,117,96,6,104,27,36,79,224,150,1,241,26,107,188,41,154,187,131,227,224,37,218,144,86,61,207,220,};
static const uint32_t old28_offsets_0[] = {4294967295u,};
static const uint32_t old28_offsets_1[] = {0u,4294967295u,};
static const XrXirFunctionLayout old28_layouts[] = {
    {1u,0u,old28_offsets_0,NULL,{16u,8u},0u,NULL,0u,0u},
    {2u,8u,old28_offsets_1,NULL,{16u,8u},0u,NULL,0u,0u},
};
static const XrXirSourceModule old28_modules[] = {
    {(const char *)(const unsigned char[]){
    0x72,0x6f,0x6f,0x74,
    0}, 4u, NULL, 0u, 0u},
};
static const XrXirFunctionIdentity old28_identities[] = {
    {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
    {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
};
static const XrXirDeclarations old28_declarations = {old28_modules, 1u, old28_identities, NULL, 0u, NULL, 0u, 0u, 1u, NULL};
_Static_assert(sizeof(XrXirFunctionIdentity) == 36, "XIR function identity stride");
_Static_assert(offsetof(XrXirFunctionIdentity, method_kind) == 24, "XIR method role offset");
_Static_assert(offsetof(XrXirFunctionIdentity, test_role) == 28, "XIR test role offset");
_Static_assert(offsetof(XrXirFunctionIdentity, test_timeout_seconds) == 32, "XIR test timeout offset");
_Static_assert(offsetof(XrXirDeclarations, implementations) == 64, "XIR implementation table offset");
_Static_assert(sizeof(XrXirDeclarations) == 72, "XIR declaration stride");
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION == 28u, "XIR program ABI");
XR_DATADEF const XrXirProgramSpec old28_program = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, old28_entries, 2u, &old28_declarations, {NULL, NULL}, &old28_types, {old28_checked, sizeof(old28_checked), old28_identity, old28_layouts}};
