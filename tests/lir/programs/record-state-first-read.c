/* Exercise the real first receiving Frame's allocation callback. */
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_values.h"
#include "xray_native_state.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
typedef struct Probe {
    XrtManagedRoot *root;
    XrtModuleState *state;
    size_t leases, root_requests, cleared, prints;
    int64_t result;
    bool armed, after;
} Probe;
static Probe *active;
static bool provider_retain(void *context) { ++((Probe *)context)->leases; return true; }
static void provider_drop(void *context) { Probe *probe = context; CHECK(probe->leases); --probe->leases; }
static void *provider_allocate(void *context, size_t bytes) {
    Probe *probe = context;
    if (probe->armed) {
        probe->armed = false;
        xrt_module_state_clear(probe->state);
        ++probe->cleared;
    }
    return xr_malloc(bytes);
}
static void provider_free(void *context, void *memory, size_t bytes) {
    (void)context; (void)bytes; xr_free(memory);
}
static XrtManagedRoot *observed_root(uint32_t source) {
    (void)source; CHECK(active && active->root); ++active->root_requests; return active->root;
}
static XrtModuleStatus observed_publish(XrtModuleRef *reference) {
    XrtModuleStatus status = xrt_module_publish(reference);
    if (status == XRT_MANAGED_OK) { CHECK(active && active->state); active->armed = true; }
    return status;
}
static void observed_print(int64_t value, uint32_t source) {
    CHECK(active); ++active->prints; xrt_print_i64(value, source);
}
#define xrt_native_managed_root observed_root
#define xrt_module_publish observed_publish
#define xrt_print_i64 observed_print
#include "GENERATED"
#undef xrt_native_managed_root
#undef xrt_module_publish
#undef xrt_print_i64
static void execute(void *context) {
    Probe *probe = context; active = probe;
    xrl_invoke(&probe->result, probe->state);
    probe->after = true;
}
int main(void) {
    XrtValueRoot values = {0}; XrtExecutionFailure failure = {0};
    XrtBillingOwner *billing = NULL, *frames = NULL;
    XrtModuleLayout *layout = NULL; XrtLocationOwner *locations = NULL;
    Probe probe = {.leases=1};
    XrtManagedAllocator allocator = {&probe, provider_allocate, provider_free,
        {&probe, provider_retain, provider_drop}};
    CHECK(xrt_billing_owner_new(&allocator, SIZE_MAX, &frames) == XRT_MANAGED_OK);
    CHECK(xrt_managed_root_new(frames, &probe.root) == XRT_MANAGED_OK);
    CHECK(xrt_native_values_prepare(&values, &billing) == XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
    CHECK(xrt_module_layout_new(billing, &xrl_state_spec, (XrtManagedLease){0}, &layout) == XRT_MANAGED_OK);
    CHECK(xrt_module_state_new(layout, billing, &probe.state) == XRT_MANAGED_OK);
    CHECK(xrt_locations_create(xrt_standalone_source_map.locations, xrt_standalone_source_map.count, &locations) == XRT_LOCATION_OK);
    XrtExecutionStatus result = xrt_run_embedded_owned(locations, &failure, execute, &probe, &values);
    CHECK(xrt_value_root_empty(&values) && probe.cleared == 1 && !probe.armed && probe.root_requests == 2);
    bool safe = result == XRT_EXECUTION_FAILURE && !probe.after && !probe.prints && failure.kind == XRT_FAILURE_VALUE &&
        failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT;
    CHECK(xrt_managed_root_destroy(&probe.root) == XRT_MANAGED_OK);
    xrt_module_state_drop(&probe.state); xrt_module_layout_drop(&layout);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    xrt_locations_release(locations); xrt_billing_owner_drop(frames); CHECK(probe.leases == 1);
    if (!safe) { fputs("State snapshot crossed its receiving Frame allocation\n", stderr); return 1; }
    return 0;
}
