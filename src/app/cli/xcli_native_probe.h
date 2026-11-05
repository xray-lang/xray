/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_native_probe.h - Owned native capability selection on one caller memory owner
 */
#ifndef XCLI_NATIVE_PROBE_H
#define XCLI_NATIVE_PROBE_H
#include "xcli_toolchain_config.h"
#include "../toolchain/xtc_xir_local_toolchain.h"

typedef struct XrCliNativeProbe XrCliNativeProbe;
typedef enum XrCliNativeProbeStatus {
    XR_CLI_NATIVE_PROBE_OK, XR_CLI_NATIVE_PROBE_INVALID, XR_CLI_NATIVE_PROBE_UNAVAILABLE,
    XR_CLI_NATIVE_PROBE_UNSUPPORTED, XR_CLI_NATIVE_PROBE_BUDGET, XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY,
    XR_CLI_NATIVE_PROBE_IO, XR_CLI_NATIVE_PROBE_INTEGRITY, XR_CLI_NATIVE_PROBE_CLEANUP,
    XR_CLI_NATIVE_PROBE_PENDING,
    XR_CLI_NATIVE_PROBE_MEMORY_LIMIT, XR_CLI_NATIVE_PROBE_OVERFLOW
} XrCliNativeProbeStatus;
typedef struct XrCliNativeProbeRequest {
    XrToolchainSelector selector;
    const char *compiler;
    bool allow_fallback;
} XrCliNativeProbeRequest;
typedef struct XrCliNativeProbeOptions {
    const XrCliToolchainConfigView *preferences;
    const char *target, *provider, *compiler;
} XrCliNativeProbeOptions;
typedef struct XrCliNativeProbeFailure {
    const char *code, *stage;
    char message[256];
} XrCliNativeProbeFailure;
typedef struct XrCliNativeProbeReport {
    const char *provider, *c_compile, *sdk_compile, *runtime_link, *native_run;
    char version[24], runtime[65], fingerprint[65];
    XrCliNativeProbeFailure failure, previous;
    bool ready, fallback_used;
    uint64_t duration_ms;
} XrCliNativeProbeReport;

/* New only allocates on the required original memory owner and preserves output on
 * failure. Run borrows request text for one synchronous capability selection.
 * Every attempt uses a real Source projection, locked compile/link replay,
 * native admission and exact native output. Explicit paths/providers,
 * resource failures and integrity failures never select another provider.
 * The current invocation owner admits Windows MSVC. Other providers
 * require their own recipe and remain explicit unsupported cases; configuration
 * fallback permission cannot grant an unavailable provider implementation. */
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_new(XrCompileResources *resources,
    XrCliNativeProbe **output);
/* Resolve the same selector/path precedence for probe and build. The result
 * borrows only the live options/snapshot and must be consumed before they die.
 * Failure preserves all output bytes. This is configuration, not readiness. */
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_resolve(const XrCliNativeProbeOptions *options, XrCliNativeProbeRequest *output);
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_run(XrCliNativeProbe *owner,
    const XrCliNativeProbeRequest *request);
/* Reports and local facts are immutable borrows until close. A local view is
 * discovery only; callers must require run OK and independently admit their
 * actual output. No probe result grants Target execution authority. */
XR_FUNC const XrCliNativeProbeReport *xr_cli_native_probe_report(const XrCliNativeProbe *owner);
XR_FUNC const XtcXirLocalToolchain *xr_cli_native_probe_local(const XrCliNativeProbe *owner);
/* A build rechecks both actual provider image identities and the SDK identity
 * after its own locked transaction. Digest equality is correspondence to a
 * fresh successful probe, never a substitute for that transaction's proofs. */
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_check_inputs(const XrCliNativeProbe *owner,
    const XrXirInvocationProviderFacts *provider, const XrXirRuntimeSdkFacts *sdk);
/* No new allocation/work. PENDING or an error preserves the cleanup owner;
 * close can be retried and NULL is already closed. */
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_close(XrCliNativeProbe **owner);
#endif // XCLI_NATIVE_PROBE_H
