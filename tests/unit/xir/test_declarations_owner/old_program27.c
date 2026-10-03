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
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#if !defined(XR_ARCH_X86_64)
#error XIR_target_mismatch
#endif
_Static_assert(XR_XIR_CALL_ABI_VERSION == 22u, "XIR call ABI");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 18u, "XIR scalar ABI");
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
typedef struct compile_owner_state_0 {
    uint32_t pc, destination, expected, normal_pc, error_pc, error_destination, panic_pc, panic_destination;
    bool initialized, waiting, invoking, discard;
    unsigned char frame[1];
} compile_owner_state_0;
XR_FUNC XrXirAction compile_owner_f0(XrXirCallView *view) {
    compile_owner_state_0 *state = view->state;
    if (view->phase == XR_XIR_CALL_EXIT)
        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};
    if (!state->initialized) {
        if (view->argument_count != 0u) goto invalid;
        state->initialized = true;
    }
    if (state->waiting) {
        state->waiting = false;
        uint32_t panic_pc = state->panic_pc; state->panic_pc = 0;
        if (xr_xir_call_panic_status(view->inbox.status)) {
            if (!panic_pc) goto invalid;
            state->invoking = false;
            return xr_xir_instance_panic_land(view, state->frame, (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, 0}, state->panic_destination, panic_pc, &state->pc);
        }
        XrXirValue inbox = view->inbox.value;
        bool discarded = false;
        if (state->invoking) {
            bool error = view->inbox.status == XR_XIR_CALL_THROWN;
            if (!error && view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;
            state->pc = error ? state->error_pc : state->normal_pc; state->invoking = false;
            if (error) { state->expected = XR_XIR_ERROR; state->destination = state->error_destination; inbox.type = XR_XIR_ERROR; }
            else if (state->discard) {
                if (xr_xir_call_discard_inbox(view, (XrXirType)state->expected) != XR_XIR_CALL_READY) goto invalid;
                discarded = true;
            }
        } else if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, inbox, {0}, 0};
        if (!discarded) {
        if (view->inbox.status != XR_XIR_CALL_RETURNED && view->inbox.status != XR_XIR_CALL_THROWN) goto invalid;
        if (state->expected == XR_XIR_UNIT) {
            if (inbox.type || inbox.reserved || inbox.payload) goto invalid;
        } else if (!xr_xir_value_argument(&inbox, view->arena, (XrXirType) state->expected)) goto invalid;
        if (xr_xir_type_is_owned(xr_xir_compile_type_arena_types(view->arena), (XrXirType) state->expected)) {
            if (xr_xir_owned_slot_copy(state->frame, state->destination, view->arena, (XrXirType) state->expected, inbox.payload) != XR_XIR_VALUE_OK) goto limit;
        } else if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(state->frame, state->destination, inbox.payload);
        }
    }
    switch (state->pc) {
    case 0u:
        state->pc = 1u;
        return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {0u, 0, 0}, {0}, 0};
    default: break;
    }
invalid:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
limit:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
}
static void compile_owner_release_0(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason; (void) view;
}
typedef struct compile_owner_state_1 {
    uint32_t pc, destination, expected, normal_pc, error_pc, error_destination, panic_pc, panic_destination;
    bool initialized, waiting, invoking, discard;
    unsigned char frame[8];
} compile_owner_state_1;
XR_FUNC XrXirAction compile_owner_f1(XrXirCallView *view) {
    compile_owner_state_1 *state = view->state;
    if (view->phase == XR_XIR_CALL_EXIT)
        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};
    if (!state->initialized) {
        if (view->argument_count != 0u) goto invalid;
        state->initialized = true;
    }
    if (state->waiting) {
        state->waiting = false;
        uint32_t panic_pc = state->panic_pc; state->panic_pc = 0;
        if (xr_xir_call_panic_status(view->inbox.status)) {
            if (!panic_pc) goto invalid;
            state->invoking = false;
            return xr_xir_instance_panic_land(view, state->frame, (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, 0}, state->panic_destination, panic_pc, &state->pc);
        }
        XrXirValue inbox = view->inbox.value;
        bool discarded = false;
        if (state->invoking) {
            bool error = view->inbox.status == XR_XIR_CALL_THROWN;
            if (!error && view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;
            state->pc = error ? state->error_pc : state->normal_pc; state->invoking = false;
            if (error) { state->expected = XR_XIR_ERROR; state->destination = state->error_destination; inbox.type = XR_XIR_ERROR; }
            else if (state->discard) {
                if (xr_xir_call_discard_inbox(view, (XrXirType)state->expected) != XR_XIR_CALL_READY) goto invalid;
                discarded = true;
            }
        } else if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, inbox, {0}, 0};
        if (!discarded) {
        if (view->inbox.status != XR_XIR_CALL_RETURNED && view->inbox.status != XR_XIR_CALL_THROWN) goto invalid;
        if (state->expected == XR_XIR_UNIT) {
            if (inbox.type || inbox.reserved || inbox.payload) goto invalid;
        } else if (!xr_xir_value_argument(&inbox, view->arena, (XrXirType) state->expected)) goto invalid;
        if (xr_xir_type_is_owned(xr_xir_compile_type_arena_types(view->arena), (XrXirType) state->expected)) {
            if (xr_xir_owned_slot_copy(state->frame, state->destination, view->arena, (XrXirType) state->expected, inbox.payload) != XR_XIR_VALUE_OK) goto limit;
        } else if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(state->frame, state->destination, inbox.payload);
        }
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
limit:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
}
static void compile_owner_release_1(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason; (void) view;
}
typedef struct compile_owner_state_2 {
    uint32_t pc, destination, expected, normal_pc, error_pc, error_destination, panic_pc, panic_destination;
    bool initialized, waiting, invoking, discard;
    unsigned char frame[8];
} compile_owner_state_2;
XR_FUNC XrXirAction compile_owner_f2(XrXirCallView *view) {
    compile_owner_state_2 *state = view->state;
    if (view->phase == XR_XIR_CALL_EXIT)
        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};
    if (!state->initialized) {
        if (view->argument_count != 0u) goto invalid;
        state->initialized = true;
    }
    if (state->waiting) {
        state->waiting = false;
        uint32_t panic_pc = state->panic_pc; state->panic_pc = 0;
        if (xr_xir_call_panic_status(view->inbox.status)) {
            if (!panic_pc) goto invalid;
            state->invoking = false;
            return xr_xir_instance_panic_land(view, state->frame, (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, 0}, state->panic_destination, panic_pc, &state->pc);
        }
        XrXirValue inbox = view->inbox.value;
        bool discarded = false;
        if (state->invoking) {
            bool error = view->inbox.status == XR_XIR_CALL_THROWN;
            if (!error && view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;
            state->pc = error ? state->error_pc : state->normal_pc; state->invoking = false;
            if (error) { state->expected = XR_XIR_ERROR; state->destination = state->error_destination; inbox.type = XR_XIR_ERROR; }
            else if (state->discard) {
                if (xr_xir_call_discard_inbox(view, (XrXirType)state->expected) != XR_XIR_CALL_READY) goto invalid;
                discarded = true;
            }
        } else if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, inbox, {0}, 0};
        if (!discarded) {
        if (view->inbox.status != XR_XIR_CALL_RETURNED && view->inbox.status != XR_XIR_CALL_THROWN) goto invalid;
        if (state->expected == XR_XIR_UNIT) {
            if (inbox.type || inbox.reserved || inbox.payload) goto invalid;
        } else if (!xr_xir_value_argument(&inbox, view->arena, (XrXirType) state->expected)) goto invalid;
        if (xr_xir_type_is_owned(xr_xir_compile_type_arena_types(view->arena), (XrXirType) state->expected)) {
            if (xr_xir_owned_slot_copy(state->frame, state->destination, view->arena, (XrXirType) state->expected, inbox.payload) != XR_XIR_VALUE_OK) goto limit;
        } else if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(state->frame, state->destination, inbox.payload);
        }
    }
    switch (state->pc) {
    case 0u:
        state->pc = 1u;
        { XrXirCallStatus status = XR_XIR_CALL_READY;
        XrXirValue value = {0};
        status = xr_xir_instance_literal(view, 0u, &value);
        if (status != XR_XIR_CALL_READY) return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};
        xr_xir_owned_slot_move(state->frame, 0u, &value);
        }
        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};
    case 1u:
        state->pc = 2u;
        return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {3u, 0, xr_xir_scalar_load(state->frame, 0u)}, {0}, 0};
    default: break;
    }
invalid:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
limit:
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
}
static void compile_owner_release_2(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason; (void) view;
    compile_owner_state_2 *state = view->state;
    xr_xir_owned_slot_clear(state->frame, 0u);
}
XR_DATADEF const XrXirCallEntry compile_owner_entries[] = {
    {XR_XIR_CALL_ABI_VERSION, NULL, 0u, (XrXirType) 0, (uint32_t) sizeof(compile_owner_state_0), compile_owner_f0, compile_owner_release_0, NULL, 0u, 0u},
    {XR_XIR_CALL_ABI_VERSION, NULL, 0u, (XrXirType) 2, (uint32_t) sizeof(compile_owner_state_1), compile_owner_f1, compile_owner_release_1, NULL, 0u, 0u},
    {XR_XIR_CALL_ABI_VERSION, NULL, 0u, (XrXirType) 3, (uint32_t) sizeof(compile_owner_state_2), compile_owner_f2, compile_owner_release_2, NULL, 0u, 0u},
};
static const uint8_t compile_owner_checked[] = {
    88,82,67,72,75,0,0,0,22,0,0,0,58,0,0,0,
    2,0,0,0,0,0,0,0,245,1,0,0,0,0,0,0,
    227,191,98,227,195,57,35,126,100,124,222,85,59,98,182,105,
    224,176,76,146,157,104,80,205,158,90,27,29,194,13,242,99,
    0,0,0,0,3,0,0,0,1,0,0,0,4,0,0,0,
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
    0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,
    116,101,120,116,0,0,0,0,3,0,0,0,1,0,0,0,
    0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,
    2,0,0,0,3,0,0,0,3,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,25,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,1,0,0,0,4,0,0,0,
    114,111,111,116,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    5,0,0,0,65,0,228,184,173,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,
};
static const uint8_t compile_owner_identity[32] = {146,36,246,70,223,88,196,25,215,145,82,179,28,254,248,241,41,15,47,176,21,59,204,245,222,39,242,141,138,219,71,224,};
static const uint32_t compile_owner_offsets_0[] = {4294967295u,};
static const uint32_t compile_owner_offsets_1[] = {0u,4294967295u,};
static const uint32_t compile_owner_offsets_2[] = {0u,4294967295u,};
static const uint32_t compile_owner_owned_2[] = {0u,};
static const XrXirFunctionLayout compile_owner_layouts[] = {
    {1u,0u,compile_owner_offsets_0,NULL,{16u,8u},0u,NULL,0u,0u},
    {2u,8u,compile_owner_offsets_1,NULL,{16u,8u},0u,NULL,0u,0u},
    {2u,8u,compile_owner_offsets_2,NULL,{16u,8u},1u,compile_owner_owned_2,0u,0u},
};
static const XrXirSourceModule compile_owner_modules[] = {
    {(const char *)(const unsigned char[]){
    0x72,0x6f,0x6f,0x74,
    0}, 4u, NULL, 0u, 0u},
};
static const XrXirFunctionIdentity compile_owner_identities[] = {
    {0u, 0u, 0u, 0u, 0u, 0u, 0u},
    {0u, 0u, 0u, 0u, 0u, 0u, 0u},
    {0u, 1u, 0u, 0u, 0u, 0u, 0u},
};
static const XrXirLiteral compile_owner_literals[] = {
    {(const char *)(const unsigned char[]){
    0x41,0x00,0xe4,0xb8,0xad,
    0}, 5u},
};
static const XrXirDeclarations compile_owner_declarations = {compile_owner_modules, 1u, compile_owner_identities, NULL, 0u, compile_owner_literals, 1u, 0u, 1u, NULL};
_Static_assert(sizeof(XrXirFunctionIdentity) == 28, "XIR function identity stride");
_Static_assert(offsetof(XrXirFunctionIdentity, method_kind) == 24, "XIR method role offset");
_Static_assert(offsetof(XrXirDeclarations, implementations) == 64, "XIR implementation table offset");
_Static_assert(sizeof(XrXirDeclarations) == 72, "XIR declaration stride");
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION == 27u, "XIR program ABI");
XR_DATADEF const XrXirProgramSpec compile_owner_program = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, compile_owner_entries, 3u, &compile_owner_declarations, {NULL, NULL}, NULL, {compile_owner_checked, sizeof(compile_owner_checked), compile_owner_identity, compile_owner_layouts}};
