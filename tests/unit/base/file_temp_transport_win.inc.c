/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_temp_transport_win.inc.c - Retained directory failure across real roots
 */
#include "lir/xlir_builder.h"
#include "vm/register/xrbc_lir.h"

typedef struct TempTransportProbe {
    const char *root;
    XrtBillingOwner *billing;
    const XrtLocationOwner *locations;
    XrtTempFailure *receiving;
    unsigned entered, returned, checked_current;
    bool unsupported, success;
} TempTransportProbe;

static const char transport_path[] = "temp-transport.xr";
static const XrtSourceLocation transport_location = {transport_path,sizeof(transport_path)-1,3,5,3,15};
static wchar_t transport_name[32768], transport_marker[32768];
static bool transport_blocked;

static void temp_transport_capture(void) {
    const DWORD length = GetFinalPathNameByHandleW(temp_child, transport_name, 32768, FILE_NAME_NORMALIZED);
    if (!length || length >= 32768) transport_name[0] = 0;
}

static void temp_transport_block(void) {
    temp_transport_capture();
    if (!transport_name[0] || !wide_join(transport_marker, transport_name, L"blocker")) return;
    const uint8_t marker = 72;
    transport_blocked = write_marker(transport_marker, &marker, 1);
    metadata_memory.cancelled = transport_blocked;
}

static bool temp_transport_current(void *context) {
    ++((TempTransportProbe *)context)->checked_current;
    return !metadata_memory.cancelled;
}

static void temp_transport_visit(void *context) { ++*(unsigned *)context; }

static int temp_transport_effect(TempTransportProbe *probe) {
    if (probe->receiving) {
        XrtExecutionFailure inner_failure;
        XrtValueRoot inner_values = {0};
        unsigned nested = 0;
        CHECK(xrt_run_embedded_receiving_temp(probe->locations, &inner_failure, temp_transport_visit,
            &nested, &inner_values, probe->receiving) == XRT_EXECUTION_INVALID && !nested);
    }
    enum { RESULT, STAGE, NAME, COUNT };
    const size_t owner = offsetof(XrtHostNullableOwner, owner);
    const size_t offsets[] = {owner, sizeof(XrtHostNullableOwner)+owner, SIZE_MAX};
    const XrtManagedFrameSpec spec = {COUNT,2*sizeof(XrtHostNullableOwner),_Alignof(XrtHostNullableOwner),offsets};
    XrtManagedRoot *managed = xrt_native_managed_root(1);
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    CHECK(xrt_managed_frame_enter_payload(managed, &spec, &frame, &payload) == XRT_MANAGED_OK);
    ++probe->entered;
    xrt_native_string_create(frame, NAME, probe->root, strlen(probe->root), 1);
    xrt_native_string_create(frame, RESULT, "previous", 8, 1);
    *(uint8_t *)payload = 1;
    const XrtManagedHeader *name = NULL, *old = NULL;
    XrtPlace result = {.frame=frame,.slot=RESULT,.nulls=XRT_PLACE_NULLABLE};
    CHECK(xrt_place_borrow((XrtPlace){.frame=frame,.slot=NAME,.nulls=XRT_PLACE_NONNULL}, &name) == XRT_MANAGED_OK);
    CHECK(xrt_place_borrow(result, &old) == XRT_MANAGED_OK && old);
    CHECK(xrt_native_values_prepare(xrt_value_root_current(1), &probe->billing) == XRT_MANAGED_OK);
    CHECK(xrt_billing_owner_retain(probe->billing));
    const XrtHostNullableSlots slots = {frame,RESULT,STAGE,0,offsets[STAGE]};
    XrtHostTempResult admitted = xrt_execution_temp_result(&slots, name, true, temp_transport_current, probe, 1);
    ++probe->returned;
    const XrtManagedHeader *after = NULL;
    probe->unsupported = admitted.host == XR_OS_IO_UNSUPPORTED && admitted.managed == XRT_MANAGED_OK;
    CHECK(xrt_place_borrow(result, &after) == XRT_MANAGED_OK && *(uint8_t *)payload == 1);
    if (probe->success) {
        CHECK(admitted.host == XR_OS_IO_OK && admitted.managed == XRT_MANAGED_OK && admitted.present);
        CHECK(xrt_backing_string_matches(after));
        const XrtStringBacking *path = (const XrtStringBacking *)after;
        size_t prefix = strlen(probe->root);
        CHECK(path->length > prefix && !memcmp(path->bytes, probe->root, prefix));
    } else CHECK(after == old);
    CHECK(xrt_managed_frame_leave(managed, frame) == XRT_MANAGED_OK);
    return 1;
}

static void temp_transport_entry(void *context) {
    if (!temp_transport_effect(context)) abort();
}

static void temp_transport_prepare(void) {
    temp_created_fixture = temp_transport_block;
    transport_blocked = false;
    metadata_memory.cancelled = false;
}

static int temp_transport_retry(TempTransportProbe *probe, XrtTempFailure *failure, unsigned attempts_before) {
    metadata_memory.cancelled = false;
    temp_created_fixture = NULL;
    CHECK(transport_blocked && probe->entered == 1 && !probe->returned && probe->checked_current);
    CHECK(failure->source_id == 1 && failure->result.managed == XRT_MANAGED_OK && !failure->result.present &&
        failure->result.host == XR_OS_IO_CANCELLED && failure->result.rollback == XR_OS_IO_IO);
    /* The root unwound a live Frame and its Strings. Only the retained
     * directory, its path storage and its retry allocation remain billed. */
    CHECK(xrt_billing_owner_stats(probe->billing).blocks == 4);
    DWORD flags = 0;
    CHECK(GetHandleInformation(temp_child, &flags));
    XrtHostTempRetry *const owner = failure->owner;
    CHECK(owner && temp_disposition_error == ERROR_DIR_NOT_EMPTY);
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(xrt_host_temp_retry(&failure->owner) == XR_OS_IO_IO && failure->owner == owner);
        CHECK(temp_disposition_error == ERROR_DIR_NOT_EMPTY && GetHandleInformation(temp_child, &flags));
    }
    CHECK(DeleteFileW(transport_marker));
    CHECK(xrt_host_temp_retry(&failure->owner) == XR_OS_IO_OK && !failure->owner);
    CHECK(GetFileAttributesW(transport_name) == INVALID_FILE_ATTRIBUTES && !GetHandleInformation(temp_child, &flags));
    CHECK(temp_create_calls == attempts_before + 1 && probe->entered == 1 && !probe->returned);
    CHECK(xrt_billing_owner_stats(probe->billing).blocks == 1);
    xrt_billing_owner_drop(probe->billing);
    return 1;
}

static int temp_transport_keep(const char *root, const XrtLocationOwner *locations) {
    XrtExecutionFailure failure;
    XrtValueRoot values = {0};
    XrtTempFailure receiver = {0};
    TempTransportProbe probe = {.root=root,.success=true};
    unsigned calls = temp_create_calls;
    temp_created_fixture = temp_transport_capture;
    XrtExecutionStatus status = xrt_run_embedded_receiving_temp(locations, &failure, temp_transport_entry, &probe,
        &values, &receiver);
    temp_created_fixture = NULL;
    CHECK(status == XRT_EXECUTION_COMPLETED && probe.returned == 1 && temp_create_calls == calls + 1);
    CHECK(xrt_billing_owner_stats(probe.billing).blocks == 1);
    xrt_billing_owner_drop(probe.billing);
    XrtTempFailure empty = {0};
    CHECK(!xrt_temp_failure_take(&receiver, &empty));
    CHECK(transport_name[0] && GetFileAttributesW(transport_name) != INVALID_FILE_ATTRIBUTES);
    CHECK(RemoveDirectoryW(transport_name));
    return 1;
}

static void temp_transport_ordinary_failure(void *context) {
    temp_transport_visit(context);
    xrt_value_failure(XR_XIR_VALUE_BOUNDS, 1);
}

static int temp_transport_embedded(const char *root) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(&transport_location, 1, &locations) == XRT_LOCATION_OK);
    XrtExecutionFailure failure;
    TempTransportProbe probe = {.root=root};
    unsigned attempts = temp_create_calls;
    temp_transport_prepare();
    CHECK(xrt_run_embedded(locations, &failure, temp_transport_entry, &probe) == XRT_EXECUTION_COMPLETED);
    CHECK(probe.unsupported && probe.entered == 1 && probe.returned == 1 && !probe.checked_current &&
        temp_create_calls == attempts && !transport_blocked);
    xrt_billing_owner_drop(probe.billing);

    XrtTempFailure receiver = {0};
    probe = (TempTransportProbe){.root=root,.locations=locations,.receiving=&receiver};
    XrtValueRoot values = {0};
    CHECK(xrt_run_embedded_receiving_temp(locations, &failure, temp_transport_entry, &probe, &values, &receiver) ==
        XRT_EXECUTION_FAILURE);
    CHECK(xrt_value_root_empty(&values) && failure.kind == XRT_FAILURE_TEMP);
    const XrtHostTempRetry *owner = receiver.owner;
    CHECK(owner && xrt_run_embedded_receiving_temp(locations, &failure, temp_transport_entry, &probe, &values, &receiver) ==
        XRT_EXECUTION_INVALID);

    /* The ordinary output can be overwritten and dropped without retiring
     * the distinct temporary-directory obligation. */
    unsigned another = 0;
    CHECK(xrt_run_embedded(locations, &failure, temp_transport_ordinary_failure, &another) == XRT_EXECUTION_FAILURE);
    CHECK(another == 1 && failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BOUNDS);
    xrt_execution_failure_drop(&failure);
    CHECK(receiver.owner == owner);

    const XrtSourceLocation *source = xrt_locations_get(locations, receiver.source_id);
    CHECK(source && source->line == 3 && source->column == 5 && source->end_column == 15 &&
        !memcmp(source->path, transport_path, sizeof(transport_path)-1));
    XrtTempFailure taken = {0};
    CHECK(xrt_temp_failure_take(&receiver, &taken) && taken.owner == owner && !receiver.owner);
    CHECK(!xrt_temp_failure_take(&receiver, &taken));
    CHECK(temp_transport_retry(&probe, &taken, attempts));
    CHECK(temp_transport_keep(root, locations));
    xrt_locations_release(locations);
    return 1;
}

static TempTransportProbe *transport_output;

/* Private host fixture reached through a real compiled scalar print. This
 * test executable supplies the output provider without changing VM dispatch. */
XR_FUNC XrtIoResult xrt_io_flush_stdout(void) { return (XrtIoResult){0}; }
XR_FUNC XrtIoResult xrt_io_write(unsigned stream, const char *bytes, size_t length) {
    if (transport_output && stream == 1 && length == 3 && !memcmp(bytes, "42\n", 3))
        temp_transport_entry(transport_output);
    return (XrtIoResult){0};
}

static int temp_transport_program(XrRbcProgram **program) {
    XrLirModule *module = NULL;
    CHECK(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    XrLirFunction root = {0}, print = {0};
    XrLirFunctionSpec spec = {.name="root",.name_length=4,.result=XR_LIR_UNIT};
    CHECK(xr_lir_declare_function(module, &spec, &root) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module, root) == XR_LIR_OK);
    const XrLirType i64 = XR_LIR_I64;
    spec = (XrLirFunctionSpec){.name="xrt_print_i64",.name_length=13,.parameters=&i64,.parameter_count=1,
        .result=XR_LIR_UNIT,.kind=XR_LIR_FUNCTION_EXTERNAL,.extern_kind=XR_LIR_EXTERN_PRINT_I64};
    CHECK(xr_lir_declare_function(module, &spec, &print) == XR_LIR_OK);
    XrLirBlock block = {0};
    XrLirValue value = {0};
    CHECK(xr_lir_add_block(module, root, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module, root, block) == XR_LIR_OK);
    CHECK(xr_lir_emit_value(module, block, &(XrLirInstruction){.op=XR_LIR_CONST,.type=i64,.bits=42}, &value) == XR_LIR_OK);
    CHECK(xr_lir_emit_effect(module, block, &(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=print,
        .operands=&value,.operand_count=1,.source={transport_path,sizeof(transport_path)-1,3,5,3,15}}) == XR_LIR_OK);
    CHECK(xr_lir_set_terminator(module, block, &(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    CHECK(xr_rbc_compile(module, root, program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    return 1;
}

static int temp_transport_rbc(const char *root) {
    XrRbcProgram *program = NULL;
    CHECK(temp_transport_program(&program));
    XrRbcExecution *plain = NULL, *receiving = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &plain) == XR_RBC_OK);
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &receiving) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    TempTransportProbe probe = {.root=root};
    const unsigned attempts = temp_create_calls;
    temp_transport_prepare();
    transport_output = &probe;
    XrRbcOutcome *outcome = xr_rbc_execution_run(plain);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK && probe.unsupported &&
        probe.returned == 1 && !probe.checked_current && temp_create_calls == attempts);
    CHECK(xr_rbc_outcome_free(&outcome) && !outcome);
    xr_rbc_execution_free(&plain);
    xrt_billing_owner_drop(probe.billing);

    probe = (TempTransportProbe){.root=root};
    outcome = xr_rbc_execution_run_receiving_temp(receiving);
    transport_output = NULL;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_TEMP_FAILURE);
    CHECK(!xr_rbc_execution_run_receiving_temp(receiving));
    xr_rbc_execution_free(&receiving);
    XrRbcOutcome *same = outcome;
    CHECK(!xr_rbc_outcome_free(&outcome) && outcome == same);
    const XrtSourceLocation *source = xr_rbc_outcome_temp_location(outcome);
    CHECK(source && source->line == 3 && source->column == 5 && source->end_column == 15 &&
        !memcmp(source->path, transport_path, sizeof(transport_path)-1));
    XrtTempFailure taken = {0};
    CHECK(xr_rbc_outcome_take_temp(outcome, &taken) && taken.owner);
    CHECK(xr_rbc_outcome_temp_location(outcome) == source && !xr_rbc_outcome_take_temp(outcome, &taken));
    CHECK(xr_rbc_outcome_free(&outcome) && !outcome);
    CHECK(temp_transport_retry(&probe, &taken, attempts));
    puts("temporary directory failure: owned C and RBC handoff across active Frame retirement");
    return 1;
}
