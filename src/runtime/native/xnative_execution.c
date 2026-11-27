/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_execution.c - Scoped image binding and synchronous output recovery
 *
 * KEY CONCEPT:
 *   Bindings stay immutable while active. Only caller-prepared failure data
 *   changes across recovery, so no changed automatic owner is lost to a jump.
 */
#include "xnative_execution.h"
#include "xnative_host_io.h"
#include "xnative_module_state.h"
#include "xnative_status.h"
#include "xnative_values.h"
#include "xray_native_values.h"
#include "xray_native_string_value.h"
#include "xray_native_pod.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include "shared/xerror_codes.h"
#include "shared/xr_time_offset.h"
#include "shared/xr_value_format_core.h"
#include "shared/xr_float_to_decimal.h"
#include "shared/xr_range_core.h"
#include <setjmp.h>

typedef struct XrtExecutionBinding {
    XrtSourceMap map;
    const struct XrtExecutionBinding *previous;
    jmp_buf *target;
    XrtExecutionFailure *failure;
    XrtValueRoot *strings;
    const XrtHostExecutionControl *host_control;
    XrtTempFailure *temp;
} XrtExecutionBinding;

static XR_THREAD_LOCAL const XrtExecutionBinding *current_execution;

XR_FUNC const XrtHostExecutionControl *xrt_execution_host_control(void) {
    return current_execution ? current_execution->host_control : NULL;
}

typedef struct XrtErrorScalar {
    uint64_t bits;
    XrPodScalar type;
} XrtErrorScalar;

struct XrtErrorSnapshot {
    XrtBillingOwner *billing;
    size_t allocation;
    XrtSourceLocation location;
    const char *name, *variant;
    uint32_t name_size, variant_size, selected, fields;
    const uint8_t *metadata;
    size_t metadata_size;
    XrtErrorScalar *values;
};
_Static_assert(_Alignof(XrtErrorSnapshot)>=_Alignof(XrtErrorScalar),"error field alignment");

typedef struct XrtErrorCursor {
    const uint8_t *next;
    size_t remaining;
} XrtErrorCursor;

static bool error_u32(XrtErrorCursor *cursor,uint32_t *output) {
    if (cursor->remaining<4) return false;
    *output=xr_error_metadata_u32(cursor->next);
    cursor->next+=4;
    cursor->remaining-=4;
    return true;
}

static bool error_name(XrtErrorCursor *cursor,const char **text,uint32_t *length) {
    if (!error_u32(cursor,length) || !*length || *length>cursor->remaining) return false;
    *text=(const char *)cursor->next;
    cursor->next+=*length;
    cursor->remaining-=*length;
    return true;
}

static uint64_t error_scalar_bits(const uint8_t *bytes,uint32_t size) {
    if (size==1) return bytes[0];
    if (size==2) { uint16_t value; memcpy(&value,bytes,sizeof(value)); return value; }
    if (size==4) { uint32_t value; memcpy(&value,bytes,sizeof(value)); return value; }
    uint64_t value; memcpy(&value,bytes,sizeof(value)); return value;
}

static bool error_field(XrtErrorCursor *cursor,size_t size,const uint8_t *value,XrtErrorScalar *field) {
    uint32_t type=0,offset=0,length=0;
    const char *name=NULL;
    XrPodLayout layout={0};
    if (!error_u32(cursor,&type) || !error_u32(cursor,&offset) || !error_name(cursor,&name,&length) ||
        !xr_pod_scalar_layout((XrPodScalar)type,&layout) || !layout.size ||
        offset>size || layout.size>size-offset || offset%layout.alignment) return false;
    if (field) {
        uint64_t bits=error_scalar_bits(value+offset,layout.size);
        if (type==XR_POD_BOOL && bits>1) return false;
        *field=(XrtErrorScalar){bits,(XrPodScalar)type};
    }
    return true;
}

static XrtManagedStatus error_snapshot_create(XrtValueRoot *root,const XrtSourceLocation *location,
    const uint8_t *metadata,size_t metadata_size,const uint8_t *value,size_t value_size,XrtErrorSnapshot **output) {
    uint32_t version=0,key=0,size=0,tag=0,variants=0,arguments=0,name_size=0,variant_size=0;
    const char *module=NULL,*name=NULL,*variant=NULL;
    XrtErrorCursor cursor={metadata,metadata_size};
    if (!metadata || !value || !error_u32(&cursor,&version) || version!=XR_ERROR_METADATA_VERSION ||
        !error_u32(&cursor,&key) || !key || !error_u32(&cursor,&size) || size!=value_size ||
        !error_u32(&cursor,&tag) || !error_u32(&cursor,&variants) || !variants ||
        tag!=(variants==1 ? 0u : variants<=256 ? 1u : variants<=65536 ? 2u : 4u) ||
        tag>size || !error_u32(&cursor,&arguments) || !error_name(&cursor,&module,&name_size) ||
        !error_name(&cursor,&name,&name_size) || arguments>cursor.remaining/8) return XRT_MANAGED_ARGUMENT;
    cursor.next+=(size_t)arguments*8;
    cursor.remaining-=(size_t)arguments*8;
    uint32_t selected=tag ? (uint32_t)error_scalar_bits(value,tag) : 0;
    if (selected>=variants) return XRT_MANAGED_ARGUMENT;
    uint32_t active_fields=0;
    XrtErrorCursor active={0};
    for (uint32_t v=0;v<variants;++v) {
        const char *label=NULL;
        uint32_t label_size=0,count=0;
        if (!error_name(&cursor,&label,&label_size) || !error_u32(&cursor,&count)) return XRT_MANAGED_ARGUMENT;
        if (v==selected) { active=cursor; active_fields=count; variant=label; variant_size=label_size; }
        for (uint32_t f=0;f<count;++f)
            if (!error_field(&cursor,value_size,NULL,NULL)) return XRT_MANAGED_ARGUMENT;
    }
    if (cursor.remaining) return XRT_MANAGED_ARGUMENT;
    if (active_fields && sizeof(XrtErrorScalar)>(SIZE_MAX-sizeof(XrtErrorSnapshot))/active_fields)
        return XRT_MANAGED_OVERFLOW;
    size_t allocation=sizeof(XrtErrorSnapshot)+(size_t)active_fields*sizeof(XrtErrorScalar);
    size_t path_size=location ? location->path_length : 0;
    if (metadata_size>SIZE_MAX-allocation || path_size>SIZE_MAX-allocation-metadata_size)
        return XRT_MANAGED_OVERFLOW;
    allocation+=metadata_size+path_size;
    XrtBillingOwner *billing=NULL;
    XrtManagedStatus status=xrt_native_values_prepare(root,&billing);
    if (status!=XRT_MANAGED_OK) return status;
    if (!xrt_billing_owner_retain(billing)) return XRT_MANAGED_REFCOUNT;
    void *storage=NULL;
    status=xrt_managed_metadata_allocate(billing,allocation,&storage);
    if (status!=XRT_MANAGED_OK) { xrt_billing_owner_drop(billing); return status; }
    XrtErrorSnapshot *failure=storage;
    XrtErrorScalar *values=(XrtErrorScalar *)(failure+1);
    uint8_t *copy=(uint8_t *)(values+active_fields);
    *failure=(XrtErrorSnapshot){.billing=billing,.allocation=allocation,
        .name=(const char *)copy+(name-(const char *)metadata),.name_size=name_size,
        .variant=(const char *)copy+(variant-(const char *)metadata),.variant_size=variant_size,
        .selected=selected,.fields=active_fields,.metadata=copy,.metadata_size=metadata_size,.values=values};
    memcpy(copy,metadata,metadata_size);
    if (location) {
        char *path=(char *)(copy+metadata_size);
        if (path_size) memcpy(path,location->path,path_size);
        failure->location=*location;
        failure->location.path=path;
    }
    for (uint32_t f=0;f<active_fields;++f)
        if (!error_field(&active,value_size,value,&failure->values[f])) {
            xrt_managed_metadata_free(billing,failure,allocation);
            xrt_billing_owner_drop(billing);
            return XRT_MANAGED_ARGUMENT;
        }
    *output=failure;
    return XRT_MANAGED_OK;
}

XRAY_API_NORET void xrt_try_assert_panic(const uint8_t *metadata,size_t metadata_size,const void *error,
    size_t error_size,uint32_t source_id) {
    const XrtExecutionBinding *binding=current_execution;
    XR_CHECK(binding && binding->target && source_id && source_id<=binding->map.count,
        "Try assertion panic uses the true source location");
    XrtTryAssertFailure *failure=NULL;
    XrtManagedStatus status=error_snapshot_create(binding->strings,&binding->map.locations[source_id-1],
        metadata,metadata_size,error,error_size,&failure);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
    binding->failure->kind=XRT_FAILURE_TRY_ASSERT;
    binding->failure->detail.assertion=failure;
    longjmp(*binding->target,1);
}

XRAY_API_NORET void xrt_uncaught_error(const uint8_t *metadata,size_t metadata_size,const void *error,
    size_t error_size) {
    const XrtExecutionBinding *binding=current_execution;
    XR_CHECK(binding && binding->target,"Uncaught errors require an active execution root");
    XrtErrorSnapshot *failure=NULL;
    XrtManagedStatus status=error_snapshot_create(binding->strings,NULL,metadata,metadata_size,
        error,error_size,&failure);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,0);
    *binding->failure=(XrtExecutionFailure){.kind=XRT_FAILURE_UNCAUGHT_ERROR,.detail.error=failure};
    longjmp(*binding->target,1);
}

XR_FUNC XR_NORET void xrt_value_failure_publish(XrtValueFailure failure) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && binding->target && failure.status != XR_XIR_VALUE_OK &&
        failure.source_id <= binding->map.count && failure.directory_close_source_id <= binding->map.count,
        "Value failure retains the actual root status and source ids");
    *binding->failure = (XrtExecutionFailure){.kind=XRT_FAILURE_VALUE,.detail.value=failure};
    longjmp(*binding->target, 1);
}
XR_FUNC XR_NORET void xrt_value_failure(XrXirValueStatus status, uint32_t source_id) {
    xrt_value_failure_publish((XrtValueFailure){.status=status,.source_id=source_id});
}
static XR_NORET void native_failure_exit(const XrtSourceMap *map, XrtExecutionFailure *failure);
XR_FUNC void xrt_execution_failure_drop(XrtExecutionFailure *failure) {
    if (!failure) return;
    XrtManagedHeader *message = failure->kind == XRT_FAILURE_MESSAGE_PANIC ? failure->detail.message.message : NULL;
    XrtErrorSnapshot *error = NULL;
    if (failure->kind == XRT_FAILURE_TRY_ASSERT) error = failure->detail.assertion;
    if (failure->kind == XRT_FAILURE_UNCAUGHT_ERROR) error = failure->detail.error;
    *failure = (XrtExecutionFailure){0};
    xrt_managed_drop(message);
    if (error) {
        XrtBillingOwner *billing=error->billing;
        xrt_managed_metadata_free(billing,error,error->allocation);
        xrt_billing_owner_drop(billing);
    }
}
XR_FUNC bool xrt_temp_failure_take(XrtTempFailure *received, XrtTempFailure *empty) {
    if (!received || !received->owner || !empty || empty->owner) return false;
    *empty = *received;
    received->owner = NULL;
    return true;
}
XR_FUNC XrtHostTempResult xrt_execution_receiving_admission(uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    if (!binding || !binding->temp) return (XrtHostTempResult){.host=XR_OS_IO_UNSUPPORTED};
    if (source_id > binding->map.count || binding->temp->owner)
        return (XrtHostTempResult){.managed=XRT_MANAGED_ARGUMENT};
    return (XrtHostTempResult){0};
}
XR_FUNC XrtHostLinkResult xrt_execution_link_result(const XrtHostNullableSlots *output,
    const XrtManagedHeader *path, bool follow, uint32_t source_id) {
    XrtHostTempResult admitted = xrt_execution_receiving_admission(source_id);
    if (admitted.managed != XRT_MANAGED_OK)
        return (XrtHostLinkResult){.managed=admitted.managed};
    /* An absent receiving binding has no place to publish a present answer,
     * so the query must not run at all: treat it as an argument failure rather
     * than as the host's empty optional. */
    if (admitted.host != XR_OS_IO_OK)
        return (XrtHostLinkResult){.managed=XRT_MANAGED_ARGUMENT};
    return xrt_host_link_text_publish(output, path, follow, source_id);
}

XR_FUNC XR_NORET void xrt_execution_temp_failure_take(XrtTempFailure *owned) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && binding->temp && !binding->temp->owner &&
        owned && owned->owner && !owned->result.present && owned->result.rollback != XR_OS_IO_OK &&
        owned->source_id <= binding->map.count,
        "the synchronous temporary-path leaf transfers to its admitted receiving binding");
    *binding->temp = *owned;
    owned->owner = NULL;
    *binding->failure = (XrtExecutionFailure){.kind=XRT_FAILURE_TEMP, .temp_source=owned->source_id};
    longjmp(*binding->target, 1);
}
XR_FUNC XR_NORET void xrt_message_panic_take(XrtManagedHeader **owned, uint32_t code, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && binding->target && (code == 434 || code == 445) && source_id <= binding->map.count && owned && *owned,
        "message panic consumes a genuine String owner at its active source point");
    XrtManagedHeader *message = *owned; *owned = NULL;
    binding->failure->kind = XRT_FAILURE_MESSAGE_PANIC;
    binding->failure->detail.message.code = code;
    binding->failure->detail.message.source_id = source_id;
    binding->failure->detail.message.message = message;
    longjmp(*binding->target, 1);
}
XRAY_API void xrt_assert_condition(uint8_t condition, const XrtStringBacking *message, uint32_t source_id) {
    XR_CHECK(condition <= 1 && message, "assertion operands are an evaluated Bool and genuine String borrow");
    if (condition) return;
    XrtManagedHeader *owned = (XrtManagedHeader *)&message->managed;
    if (!xrt_managed_retain(owned)) xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, source_id);
    xrt_message_panic_take(&owned, 445, source_id);
}
XRAY_API_NORET void xrt_fixed_array_bounds(uint32_t source_id) {
    xrt_value_failure(XR_XIR_VALUE_BOUNDS, source_id);
}

typedef struct XrtExecutionHooks {
    XrtMetadataCloser close_metadata;
    const XrtHostExecutionControl *control;
    XrtTempFailure *temp;
    const XrtTestRetirementHook *retirement;
} XrtExecutionHooks;

/* Mutable managed-value state belongs to the caller of this setjmp function. */
static XrtExecutionStatus run_bound(const XrtSourceMap *map, XrtExecutionFailure *failure,
    XrtSynchronousRoot root, void *context, XrtValueRoot *strings, XrtExecutionHooks hooks) {
    jmp_buf target;
    const XrtExecutionBinding binding = {.map = *map, .previous = current_execution,
        .target = &target, .failure = failure, .strings = strings,
        .host_control = hooks.control, .temp = hooks.temp};
    current_execution = &binding;
    const XrtValueBinding previous_strings = xrt_value_root_bind((XrtValueBinding){strings, map->count});
    XrtExecutionStatus result;
    if (setjmp(target) == 0) {
        root(context);
        result = XRT_EXECUTION_COMPLETED;
    } else result = XRT_EXECUTION_FAILURE;
    if (hooks.retirement && hooks.retirement->retiring)
        hooks.retirement->retiring(hooks.retirement->context);
    XrOsIoStatus retired = xrt_value_root_drain_with(strings, hooks.close_metadata);
    current_execution = binding.previous;
    (void)xrt_value_root_bind(previous_strings);
    if (result == XRT_EXECUTION_COMPLETED && retired != XR_OS_IO_OK) {
        *failure = (XrtExecutionFailure){.kind=XRT_FAILURE_VALUE,
            .detail.value={.status=xrt_host_io_failure(retired)}};
        return XRT_EXECUTION_FAILURE;
    }
    return result;
}

static bool source_map_valid(const XrtSourceMap *map) {
    if (!map || (map->count == 0) != (map->locations == NULL) ||
        map->count > UINT32_MAX || map->count > SIZE_MAX / sizeof(*map->locations))
        return false;
    for (size_t i = 0; i < map->count; ++i) {
        const XrtSourceLocation *location = &map->locations[i];
        if (!location->path || !location->path_length || location->path_length == SIZE_MAX ||
            !location->line || !location->column)
            return false;
        if (!location->end_line && !location->end_column) continue;
        if (!location->end_line || !location->end_column || location->end_line < location->line ||
            (location->end_line == location->line && location->end_column < location->column))
            return false;
    }
    return true;
}

XRAY_API void xrt_run_standalone_receiving_temp(const XrtSourceMap *map, XrtSynchronousRoot root,
    void *context) {
    XR_CHECK(root && source_map_valid(map), "A receiving standalone root requires a valid prepared map");
    XrtValueRoot strings = {0};
    XrtExecutionFailure failure = {0};
    XrtTempFailure receiver = {0};
    const XrtExecutionHooks hooks = {.close_metadata=xr_os_io_file_metadata_close, .temp=&receiver};
    if (run_bound(map, &failure, root, context, &strings, hooks) == XRT_EXECUTION_FAILURE)
        native_failure_exit(map, &failure);
    /* A completed run owns no rollback obligation. */
    XR_CHECK(!receiver.owner, "a completed standalone run holds no temporary rollback");
}
XRAY_API void xrt_run_standalone(const XrtSourceMap *map, XrtSynchronousRoot root, void *context) {
    XR_CHECK(root && source_map_valid(map), "A standalone root requires a valid prepared source map");
    XrtValueRoot strings = {0};
    XrtExecutionFailure failure = {0};
    /* A failed drain leaves the handle owned until standalone process exit. */
    const XrtExecutionHooks hooks = {.close_metadata=xr_os_io_file_metadata_close};
    if (run_bound(map, &failure, root, context, &strings, hooks) == XRT_EXECUTION_FAILURE)
        native_failure_exit(map, &failure);
}

typedef struct XrtStateInvocation {
    XrtSynchronousStateRoot root;
    void *context;
    XrtModuleState *state;
} XrtStateInvocation;

static void state_root_invoke(void *context) {
    const XrtStateInvocation *invocation = context;
    invocation->root(invocation->context, invocation->state);
}

XRAY_API void xrt_run_standalone_state(const XrtSourceMap *map, XrtSynchronousStateRoot root,
    void *context, const XrtModuleLayoutSpec *spec) {
    XR_CHECK(root && spec && source_map_valid(map), "A state root requires a prepared map and layout");
    XrtValueRoot values = {0};
    XrtBillingOwner *billing = NULL;
    XrtModuleLayout *layout = NULL;
    XrtModuleState *state = NULL;
    XrtModuleStatus status = xrt_native_values_prepare(&values, &billing);
    if (status == XRT_MANAGED_OK)
        status = xrt_module_layout_new(billing, spec, (XrtManagedLease){0}, &layout);
    if (status == XRT_MANAGED_OK) status = xrt_module_state_new(layout, billing, &state);
    xrt_module_layout_drop(&layout);
    XrtExecutionFailure failure = {0};
    if (status != XRT_MANAGED_OK) {
        xrt_module_state_drop(&state);
        (void)xrt_value_root_drain(&values);
        failure.kind = XRT_FAILURE_VALUE;
        failure.detail.value.status = xrt_managed_value_status(status);
        native_failure_exit(map, &failure);
    }
    xrt_native_values_use_state(&values, state, spec->values);
    XrtStateInvocation invocation = {root, context, state};
    const XrtExecutionHooks hooks = {.close_metadata=xr_os_io_file_metadata_close};
    XrtExecutionStatus result = run_bound(map, &failure, state_root_invoke, &invocation, &values, hooks);
    xrt_module_state_drop(&state);
    if (result == XRT_EXECUTION_FAILURE) native_failure_exit(map, &failure);
}

XRAY_API void xrt_run_standalone_state_receiving_temp(const XrtSourceMap *map, XrtSynchronousStateRoot root,
    void *context, const XrtModuleLayoutSpec *spec) {
    XR_CHECK(root && spec && source_map_valid(map), "A receiving state root requires a prepared map and layout");
    XrtValueRoot values = {0};
    XrtBillingOwner *billing = NULL;
    XrtModuleLayout *layout = NULL;
    XrtModuleState *state = NULL;
    XrtModuleStatus status = xrt_native_values_prepare(&values, &billing);
    if (status == XRT_MANAGED_OK)
        status = xrt_module_layout_new(billing, spec, (XrtManagedLease){0}, &layout);
    if (status == XRT_MANAGED_OK) status = xrt_module_state_new(layout, billing, &state);
    xrt_module_layout_drop(&layout);
    XrtExecutionFailure failure = {0};
    if (status != XRT_MANAGED_OK) {
        xrt_module_state_drop(&state);
        (void)xrt_value_root_drain(&values);
        failure.kind = XRT_FAILURE_VALUE;
        failure.detail.value.status = xrt_managed_value_status(status);
        native_failure_exit(map, &failure);
    }
    xrt_native_values_use_state(&values, state, spec->values);
    XrtStateInvocation invocation = {root, context, state};
    XrtTempFailure receiver = {0};
    const XrtExecutionHooks hooks = {.close_metadata=xr_os_io_file_metadata_close, .temp=&receiver};
    XrtExecutionStatus result = run_bound(map, &failure, state_root_invoke, &invocation, &values, hooks);
    xrt_module_state_drop(&state);
    if (result == XRT_EXECUTION_FAILURE) native_failure_exit(map, &failure);
    XR_CHECK(!receiver.owner, "a completed standalone run holds no temporary rollback");
}

XR_FUNC XrtExecutionStatus xrt_run_embedded(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context) {
    XrtValueRoot strings = {.implicit=true};
    XrtExecutionStatus status = xrt_run_embedded_owned(locations, failure, root, context, &strings);
    XR_CHECK(xrt_value_root_empty(&strings), "a local execution cannot retain a host handle past its lifetime");
    return status;
}
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings) {
    return xrt_run_embedded_owned_controlled(locations, failure, root, context, strings, NULL);
}
static XrtExecutionStatus run_embedded_owned(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings,
    XrtExecutionHooks hooks) {
    if (!locations || !failure || !root || !strings || !hooks.close_metadata ||
        (hooks.control && !hooks.control->current) || (hooks.temp && hooks.temp->owner) ||
        !xrt_native_values_prepared(strings))
        return XRT_EXECUTION_INVALID;
    for (const XrtExecutionBinding *active = current_execution; active; active = active->previous)
        if (active->strings == strings || (hooks.temp && active->temp == hooks.temp))
            return XRT_EXECUTION_INVALID;
    XrtSourceMap map = {xrt_locations_get(locations, 1), xrt_locations_count(locations)};
    return run_bound(&map, failure, root, context, strings, hooks);
}
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned_controlled(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings,
    const XrtHostExecutionControl *control) {
    const XrtExecutionHooks hooks = {.close_metadata=xr_os_io_file_metadata_close, .control=control};
    return run_embedded_owned(locations, failure, root, context, strings, hooks);
}
XR_FUNC XrtExecutionStatus xrt_run_embedded_owned_with_metadata_close(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings,
    XrtMetadataCloser close_metadata) {
    const XrtExecutionHooks hooks = {.close_metadata=close_metadata};
    return run_embedded_owned(locations, failure, root, context, strings, hooks);
}

XR_FUNC XrtExecutionStatus xrt_run_embedded_test_action(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings,
    XrtMetadataCloser close_metadata, const XrtTestRetirementHook *retirement) {
    const XrtExecutionHooks hooks = {.close_metadata=close_metadata, .retirement=retirement};
    return run_embedded_owned(locations, failure, root, context, strings, hooks);
}

struct XrtTestSession {
    XrtModuleLayout *layout;
    XrtModuleState *state;
    XrtValueRoot native_root;
    const XrtNativeTestFile *native_file;
    XrtLocationOwner *locations;
    XrtMetadataCloser close_metadata;
    bool active, initialized, initialization_failed;
};

XR_FUNC XrtManagedStatus xrt_test_session_create(const XrtModuleLayoutSpec *spec, XrtManagedLease code,
    XrtTestSession **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    XrtTestSession *session = xr_calloc(1, sizeof(*session));
    if (!session) return XRT_MANAGED_OOM;
    XrtManagedStatus status = XRT_MANAGED_OK;
    if (spec) {
        XrtBillingOwner *billing = NULL;
        status = xrt_billing_owner_new(NULL, SIZE_MAX, &billing);
        if (status == XRT_MANAGED_OK)
            status = xrt_module_layout_new(billing, spec, code, &session->layout);
        if (status == XRT_MANAGED_OK)
            status = xrt_module_state_new(session->layout, billing, &session->state);
        if (billing) xrt_billing_owner_drop(billing);
    }
    if (status != XRT_MANAGED_OK) {
        xrt_test_session_free(&session);
        return status;
    }
    *output = session;
    return XRT_MANAGED_OK;
}

XR_FUNC XrtExecutionStatus xrt_test_session_run(XrtTestSession *session, XrtTestSessionStep step,
    void *context) {
    if (!session || session->active || session->initialization_failed || !step)
        return XRT_EXECUTION_INVALID;
    session->active = true;
    XrtExecutionStatus status = XRT_EXECUTION_COMPLETED;
    if (!session->initialized) {
        status = step(context, session->layout, session->state, true);
        session->initialized = status == XRT_EXECUTION_COMPLETED;
        session->initialization_failed = !session->initialized;
    }
    if (status == XRT_EXECUTION_COMPLETED)
        status = step(context, session->layout, session->state, false);
    session->active = false;
    return status;
}

XR_FUNC XrOsIoStatus xrt_test_session_free(XrtTestSession **owner) {
    if (!owner || !*owner) return XR_OS_IO_OK;
    XrtTestSession *session = *owner;
    if (session->active) return XR_OS_IO_BAD_ARGUMENT;
    XrOsIoStatus status = session->close_metadata ?
        xrt_value_root_drain_with(&session->native_root, session->close_metadata) : XR_OS_IO_OK;
    if (status != XR_OS_IO_OK) return status;
    *owner = NULL;
    xrt_module_state_drop(&session->state);
    xrt_module_layout_drop(&session->layout);
    xrt_locations_release(session->locations);
    xr_free(session);
    return XR_OS_IO_OK;
}

XR_FUNC bool xrt_test_session_active(const XrtTestSession *session) {
    return session && session->active;
}

typedef struct XrtNativeTestAction {
    XrtTestSession *session;
    XrtExecutionFailure *failure;
    uint32_t action;
    const XrtTestRetirementHook *hook;
} XrtNativeTestAction;

static XrtExecutionStatus native_test_step(void *context, XrtModuleLayout *layout,
    XrtModuleState *state, bool initialize) {
    (void)layout;
    XrtNativeTestAction *action = context;
    XrtTestSession *session = action->session;
    const XrtNativeTestFile *file = session->native_file;
    XrtValueRoot *root = &session->native_root;
    XrOsIoStatus closed = xrt_value_root_drain_with(root, session->close_metadata);
    if (closed != XR_OS_IO_OK) {
        *action->failure = (XrtExecutionFailure){.kind=XRT_FAILURE_VALUE,
            .detail.value={.status=xrt_host_io_failure(closed)}};
        return XRT_EXECUTION_FAILURE;
    }
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(root, &billing);
    if (status != XRT_MANAGED_OK) {
        *action->failure = (XrtExecutionFailure){.kind=XRT_FAILURE_VALUE,
            .detail.value={.status=xrt_managed_value_status(status)}};
        return XRT_EXECUTION_FAILURE;
    }
    if (state) xrt_native_values_use_state(root, state, file->state->values);
    XrtStateInvocation invocation = {
        initialize ? file->initializer : file->actions[action->action], NULL, state};
    return xrt_run_embedded_test_action(session->locations, action->failure, state_root_invoke,
        &invocation, root, session->close_metadata, initialize ? NULL : action->hook);
}

XR_FUNC XrtManagedStatus xrt_test_session_create_native_with_metadata_close(const XrtNativeTestFile *file,
    XrtMetadataCloser close_metadata, XrtTestSession **output) {
    if (!file || !file->sources || !file->initializer || !file->actions || !file->action_count ||
        !close_metadata || !output || *output) return XRT_MANAGED_ARGUMENT;
    XrtTestSession *session = NULL;
    XrtManagedStatus status = xrt_test_session_create(file->state, (XrtManagedLease){0}, &session);
    if (status != XRT_MANAGED_OK) return status;
    XrtLocationStatus located = xrt_locations_create(file->sources->locations, file->sources->count,
        &session->locations);
    if (located != XRT_LOCATION_OK) {
        xrt_test_session_free(&session);
        return located == XRT_LOCATION_OUT_OF_MEMORY ? XRT_MANAGED_OOM : XRT_MANAGED_ARGUMENT;
    }
    session->native_file = file;
    session->close_metadata = close_metadata;
    *output = session;
    return XRT_MANAGED_OK;
}

XR_FUNC XrtManagedStatus xrt_test_session_create_native(const XrtNativeTestFile *file,
    XrtTestSession **output) {
    return xrt_test_session_create_native_with_metadata_close(file, xr_os_io_file_metadata_close, output);
}

XR_FUNC XrtLocationOwner *xrt_test_session_locations(const XrtTestSession *session) {
    return session ? session->locations : NULL;
}

XR_FUNC XrtExecutionStatus xrt_test_session_run_native(XrtTestSession *session, uint32_t action,
    XrtExecutionFailure *failure) {
    return xrt_test_session_run_native_with_retirement_hook(session, action, NULL, failure);
}

XR_FUNC XrtExecutionStatus xrt_test_session_run_native_with_retirement_hook(XrtTestSession *session,
    uint32_t action, const XrtTestRetirementHook *hook, XrtExecutionFailure *failure) {
    const XrtNativeTestFile *file = session ? session->native_file : NULL;
    if (!file || action >= file->action_count || !file->actions[action] || !failure)
        return XRT_EXECUTION_INVALID;
    XrtNativeTestAction request = {session, failure, action, hook};
    return xrt_test_session_run(session, native_test_step, &request);
}
XR_FUNC XrtExecutionStatus xrt_run_embedded_receiving_temp(const XrtLocationOwner *locations,
    XrtExecutionFailure *failure, XrtSynchronousRoot root, void *context, XrtValueRoot *strings,
    XrtTempFailure *receiver) {
    if (!receiver) return XRT_EXECUTION_INVALID;
    const XrtExecutionHooks hooks = {.close_metadata=xr_os_io_file_metadata_close,
        .temp=receiver};
    return run_embedded_owned(locations, failure, root, context, strings, hooks);
}

XR_FUNC XR_NORET void xrt_end_execution(XrtIoResult failure,
    XrtOutputOperation operation, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && failure.failed, "Output failure requires an active execution");
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_OUTPUT;
        binding->failure->detail.output = (XrtOutputFailure) {failure, operation, source_id};
        longjmp(*binding->target, 1);
    }
    xrt_output_exit(&binding->map, failure, operation, source_id);
}

static int panic_write(void *context, const void *bytes, size_t length) {
    (void)context;
    return !xrt_io_write(2, bytes, length).failed;
}

static int error_snapshot_read(const void *context,XrValueFormatNode node,XrValueFormatView *view) {
    const XrtErrorSnapshot *failure=context;
    if (!node.type) {
        view->kind=XR_VALUE_FORMAT_ENUM;
        view->name=failure->name;
        view->name_size=failure->name_size;
        view->member=failure->variant;
        view->member_size=failure->variant_size;
        view->children=failure->fields;
        return 1;
    }
    if (node.type>failure->fields || node.value!=&failure->values[node.type-1]) return 0;
    XrtErrorScalar scalar=failure->values[node.type-1];
    if (scalar.type==XR_POD_BOOL) {
        view->kind=XR_VALUE_FORMAT_BOOL;
        view->unsigned_value=scalar.bits;
    } else if (scalar.type>=XR_POD_I8 && scalar.type<=XR_POD_I64) {
        XrPodLayout layout;
        if (!xr_pod_scalar_layout(scalar.type,&layout)) return 0;
        uint64_t bits=scalar.bits;
        if (layout.size<8 && bits & (UINT64_C(1)<<(layout.size*8-1)))
            bits|=UINT64_MAX<<(layout.size*8);
        memcpy(&view->signed_value,&bits,sizeof(bits));
        view->kind=XR_VALUE_FORMAT_SIGNED;
    } else if (scalar.type>=XR_POD_U8 && scalar.type<=XR_POD_U64) {
        view->kind=XR_VALUE_FORMAT_UNSIGNED;
        view->unsigned_value=scalar.bits;
    } else if (scalar.type==XR_POD_F32 || scalar.type==XR_POD_F64) {
        if (!xr_float_to_decimal(scalar.type==XR_POD_F32 ? 32u : 64u,scalar.bits,
            (char *)view->scalar_bytes,sizeof(view->scalar_bytes),&view->size)) return 0;
        view->kind=XR_VALUE_FORMAT_BYTES;
        view->bytes=view->scalar_bytes;
    } else return 0;
    return 1;
}

static int error_snapshot_child(const void *context,XrValueFormatNode node,uint32_t index,XrValueFormatNode *child) {
    const XrtErrorSnapshot *failure=context;
    if (node.type || index>=failure->fields) return 0;
    *child=(XrValueFormatNode){&failure->values[index],index+1};
    return 1;
}

XR_FUNC const XrtSourceLocation *xrt_try_assert_location(const XrtTryAssertFailure *failure) {
    return failure && failure->location.path ? &failure->location : NULL;
}

XR_FUNC bool xrt_try_assert_value(const XrtTryAssertFailure *failure,XrtTryAssertValue *output) {
    if (!failure || !output) return false;
    *output=(XrtTryAssertValue){failure->metadata,failure->metadata_size,failure->selected,failure->fields};
    return true;
}

XR_FUNC bool xrt_try_assert_field(const XrtTryAssertFailure *failure,uint32_t field,uint32_t *scalar,uint64_t *bits) {
    if (!failure || field>=failure->fields || !scalar || !bits) return false;
    *scalar=(uint32_t)failure->values[field].type;
    *bits=failure->values[field].bits;
    return true;
}

XR_FUNC bool xrt_try_assert_format(const XrtTryAssertFailure *failure, const XrValueFormatSink *sink) {
    XrValueFormatReader reader={failure,error_snapshot_read,error_snapshot_child};
    return failure && sink && xr_value_format_try_assert(reader,(XrValueFormatNode){failure,0},*sink);
}
XR_FUNC bool xrt_uncaught_error_format(const XrtErrorSnapshot *failure, const XrValueFormatSink *sink) {
    XrValueFormatReader reader={failure,error_snapshot_read,error_snapshot_child};
    return failure && sink && xr_value_format_uncaught(reader,(XrValueFormatNode){failure,0},*sink,0,0);
}
XR_FUNC bool xrt_try_assert_report(const XrtTryAssertFailure *failure) {
    return xrt_try_assert_format(failure,&(XrValueFormatSink){NULL,panic_write});
}
XR_FUNC bool xrt_uncaught_error_report(const XrtErrorSnapshot *failure) {
    return xrt_uncaught_error_format(failure,&(XrValueFormatSink){NULL,panic_write});
}
XR_FUNC bool xrt_message_panic_format(uint32_t code, const char *message, size_t length,
    const XrtSourceLocation *source, const XrValueFormatSink *sink) {
    if (!sink || !xr_value_format_panic(*sink, code, 0, 0, 0, 1, (const uint8_t *)message, length, 0)) return false;
    if (code != 445 || !source) return true;
    char position[48];
    int written = snprintf(position, sizeof(position), ":%u:%u\n", source->line, source->column);
    return written > 0 && (size_t)written < sizeof(position) &&
        xr_value_format_write(*sink, "    at ", 7) && xr_value_format_write(*sink, source->path, source->path_length) &&
        xr_value_format_write(*sink, position, (size_t)written);
}
XR_FUNC bool xrt_message_panic_report(uint32_t code, const char *message, size_t length,
    const XrtSourceLocation *source) {
    return xrt_message_panic_format(code, message, length, source, &(XrValueFormatSink){NULL, panic_write});
}
static XR_NORET void native_failure_exit(const XrtSourceMap *map, XrtExecutionFailure *failure) {
    if (failure->kind == XRT_FAILURE_TEMP) {
        uint32_t id = failure->temp_source;
        xrt_temp_failure_report(id && id <= map->count ? &map->locations[id - 1] : NULL);
        _Exit(1);
    }
    if (failure->kind == XRT_FAILURE_OUTPUT)
        xrt_output_exit(map, failure->detail.output.io,
            failure->detail.output.operation, failure->detail.output.source_id);
    if (failure->kind == XRT_FAILURE_HOST_CLOCK) xrt_clock_exit(map, failure->detail.clock);
    if (failure->kind == XRT_FAILURE_NULL_UNWRAP) {
        uint32_t id = failure->detail.panic.source_id;
        xrt_null_unwrap_report(id && id <= map->count ? &map->locations[id - 1] : NULL);
        _Exit(1);
    }
    if (failure->kind == XRT_FAILURE_VALUE) {
        uint32_t id = failure->detail.value.source_id;
        const XrtSourceLocation *source = id && id <= map->count ? &map->locations[id - 1] : NULL;
        uint32_t close_id = failure->detail.value.directory_close_source_id;
        const XrtSourceLocation *close = close_id && close_id <= map->count ? &map->locations[close_id - 1] : NULL;
        xrt_value_failure_report_with_directory_close(failure->detail.value.status, source, close);
        _Exit(1);
    }
    if (failure->kind==XRT_FAILURE_TRY_ASSERT) {
        if (!xrt_try_assert_report(failure->detail.assertion)) {
            static const char fallback[]="XR_RUN_6005: cannot render uncaught try assertion\n";
            (void)xrt_io_write(2,fallback,sizeof(fallback)-1);
        }
        xrt_execution_failure_drop(failure);
        _Exit(1);
    }
    if (failure->kind==XRT_FAILURE_UNCAUGHT_ERROR) {
        if (!xrt_uncaught_error_report(failure->detail.error)) {
            static const char fallback[]="XR_RUN_6005: cannot render uncaught error\n";
            (void)xrt_io_write(2,fallback,sizeof(fallback)-1);
        }
        xrt_execution_failure_drop(failure);
        _Exit(1);
    }
    XrValueFormatSink sink = {NULL, panic_write};
    if (failure->kind == XRT_FAILURE_MESSAGE_PANIC) {
        const XrtStringBacking *message = (const XrtStringBacking *)failure->detail.message.message;
        uint32_t id = failure->detail.message.source_id;
        const XrtSourceLocation *source = id && id <= map->count ? &map->locations[id - 1] : NULL;
        if (!xrt_message_panic_report(failure->detail.message.code, message->bytes, message->length, source)) {
            static const char fallback[] = "XR_RUN_6005: cannot render uncaught message panic\n";
            (void)xrt_io_write(2, fallback, sizeof(fallback) - 1);
        }
        xrt_execution_failure_drop(failure);
        _Exit(1);
    }
    XR_CHECK(failure->kind == XRT_FAILURE_TYPED_PANIC, "Native failure has a known physical carrier");
    if (!xr_value_format_panic(sink, failure->detail.panic.code, 0, 0, 0, 0, NULL, 0, 0)) {
        static const char fallback[] = "XR_RUN_6005: cannot render uncaught typed panic\n";
        (void)xrt_io_write(2, fallback, sizeof(fallback) - 1);
    }
    _Exit(1);
}

XRAY_API_NORET void xrt_null_unwrap(uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && binding->target && source_id <= binding->map.count,
        "Null unwrap uses the active execution and its actual source point");
    binding->failure->kind = XRT_FAILURE_NULL_UNWRAP;
    binding->failure->detail.panic = (XrtArithmeticPanic){413, source_id};
    longjmp(*binding->target, 1);
}

XRAY_API void xrt_range_check(int64_t start, int64_t end, int64_t length, uint32_t source_id) {
    if (!xr_range_valid(start, end, length)) xrt_value_failure(XR_XIR_VALUE_BOUNDS, source_id);
}

XRAY_API_NORET void xrt_typed_panic(uint32_t code, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && (code == 420 || code == 421 || code == 422 || code == 431 ||
        code == XR_ERR_STRING_SLICE_RANGE) && source_id <= binding->map.count,
        "Typed panic requires an active execution, admitted code and source id");
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_TYPED_PANIC;
        binding->failure->detail.panic = (XrtArithmeticPanic) {code, source_id};
        longjmp(*binding->target, 1);
    }
    XrValueFormatSink sink = {NULL, panic_write};
    if (!xr_value_format_panic(sink, code, 0, 0, 0, 0, NULL, 0, 0)) {
        static const char fallback[] = "XR_RUN_6005: cannot render uncaught typed panic\n";
        (void)xrt_io_write(2, fallback, sizeof(fallback) - 1);
    }
    _Exit(1);
}
XRAY_API_NORET void xrt_arithmetic_panic(uint32_t code, uint32_t source_id) {
    XR_CHECK(code == 420 || code == 421 || code == 422, "arithmetic operations use their admitted panic codes");
    xrt_typed_panic(code, source_id);
}

static XR_NORET void clock_failure(uint32_t kind, uint32_t source_id, XrtHostClockReason reason) {
    const XrtExecutionBinding *binding = current_execution;
    XrtHostClockFailure failure = {kind, source_id, reason};
    if (binding->target) {
        binding->failure->kind = XRT_FAILURE_HOST_CLOCK;
        binding->failure->detail.clock = failure;
        longjmp(*binding->target, 1);
    }
    xrt_clock_exit(&binding->map, failure);
}
static int64_t clock_read(uint32_t kind, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && kind >= 1 && kind <= 3 && source_id <= binding->map.count,
        "Clock reads require an active execution, registered kind and source id");
    uint64_t reading = kind == 1 ? xr_time_realtime_ns() : kind == 2 ?
        xr_time_process_cpu_ns() : xr_time_monotonic_ns();
    if (reading <= INT64_MAX && (reading || kind == 2)) return (int64_t)reading;
    clock_failure(kind, source_id,
        reading > INT64_MAX ? XRT_CLOCK_OUT_OF_RANGE : XRT_CLOCK_READ_FAILED);
}
XRAY_API int64_t xrt_clock_realtime_nanos(uint32_t source_id) {
    return clock_read(1, source_id);
}
XRAY_API int64_t xrt_clock_process_cpu_nanos(uint32_t source_id) {
    return clock_read(2, source_id);
}
XRAY_API int64_t xrt_clock_monotonic_nanos(uint32_t source_id) {
    return clock_read(3, source_id);
}
XRAY_API int64_t xrt_clock_utc_offset_at(int64_t seconds, uint32_t source_id) {
    const XrtExecutionBinding *binding = current_execution;
    XR_CHECK(binding && source_id <= binding->map.count,
        "UTC offset queries require an active execution and registered source id");
    int64_t minutes = 0;
    if (xr_time_utc_offset_at(seconds, &minutes)) return minutes;
    clock_failure(4, source_id, XRT_CLOCK_OUT_OF_RANGE);
}
