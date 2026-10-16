/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_native_probe.c - Probe owner exits and independent input comparison
 */
#include "app/cli/xcli_native_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir/xir_root_parameter_fixture.h"
#include "app/cli/xcli_native_probe.c"

static XrXirCompileContext native_probe_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);return context;
}
static void native_probe_balanced(XrXirCompileContext *context,uint64_t baseline) {
    CHECK(rp_stats(context).live_bytes==baseline);
    xr_compile_resources_release(context->resources);context->resources=NULL;
    CHECK(!rp_live && !rp_live_bytes);
}
static XrCliNativeProbeStatus native_probe_owner(const XrXirCompileContext *context) {
    XrCliNativeProbe *owner=NULL;
    XrCliNativeProbeStatus status=xr_cli_native_probe_new(context->resources,&owner);
    if (status!=XR_CLI_NATIVE_PROBE_OK) { CHECK(!owner);return status; }
    XrCliNativeProbe *occupied=owner;XrCompileResourceStats before=rp_stats(context);
    CHECK(xr_cli_native_probe_new(context->resources,&occupied)==XR_CLI_NATIVE_PROBE_INVALID && occupied==owner);
    CHECK(xr_cli_native_probe_run(owner,NULL)==XR_CLI_NATIVE_PROBE_INVALID);
    XrCompileResourceStats after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));
    const XrCliNativeProbeRequest request={XR_TOOLCHAIN_SELECTOR_ZIG,NULL,true};
    CHECK(xr_cli_native_probe_run(owner,&request)==XR_CLI_NATIVE_PROBE_UNSUPPORTED);
    const XrCliNativeProbeReport *report=xr_cli_native_probe_report(owner);
    CHECK(report && !report->ready && !report->fallback_used && !strcmp(report->failure.code,"TARGET_UNSUPPORTED"));
    CHECK(!xr_cli_native_probe_local(owner));
    CHECK(xr_cli_native_probe_run(owner,&request)==XR_CLI_NATIVE_PROBE_INVALID);
    CHECK(xr_cli_native_probe_close(&owner)==XR_CLI_NATIVE_PROBE_OK && !owner);
    CHECK(xr_cli_native_probe_close(&owner)==XR_CLI_NATIVE_PROBE_OK);return status;
}
static void native_probe_owner_failures(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};
    XrXirCompileContext context=native_probe_context(caps);uint64_t baseline=rp_stats(&context).live_bytes;
    size_t start=rp_attempts;CHECK(native_probe_owner(&context)==XR_CLI_NATIVE_PROBE_OK);
    size_t sites=rp_attempts-start;XrCompileResourceStats exact=rp_stats(&context);
    CHECK(sites==1 && exact.work);native_probe_balanced(&context,baseline);
    context=native_probe_context(caps);baseline=rp_stats(&context).live_bytes;
    size_t physical=rp_live,bytes=rp_live_bytes;
    rp_fail_at=rp_attempts;rp_injected=false;
    CHECK(native_probe_owner(&context)==XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY && rp_injected &&
        rp_live==physical && rp_live_bytes==bytes);
    rp_fail_at=SIZE_MAX;CHECK(native_probe_owner(&context)==XR_CLI_NATIVE_PROBE_OK);
    native_probe_balanced(&context,baseline);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.allocated_bytes=exact.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=exact.peak_bytes-minus;
        else limits.work=exact.work-minus;
        context=native_probe_context(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(native_probe_owner(&context)==(minus?XR_CLI_NATIVE_PROBE_BUDGET:XR_CLI_NATIVE_PROBE_OK));
        native_probe_balanced(&context,baseline);
    }
}
/* These are comparison-kernel inputs only. Actual readiness and locked build
 * transactions are exercised by the public CLI integration registrations. */
static void native_probe_input_comparison(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};
    XrXirCompileContext context=native_probe_context(caps);uint64_t baseline=rp_stats(&context).live_bytes;
    XrCliNativeProbe owner={0};owner.resources=context.resources;owner.ran=true;owner.report.ready=true;
    owner.compiler_length=123;owner.linker_length=456;
    XrXirInvocationProviderFacts provider={0};provider.compiler.length=123;provider.linker.length=456;
    XrXirRuntimeSdkFacts sdk={0};size_t physical=rp_live,bytes=rp_live_bytes;
    CHECK(xr_cli_native_probe_check_inputs(&owner,&provider,&sdk)==XR_CLI_NATIVE_PROBE_OK);
    for (unsigned field=0;field<3;++field) for (unsigned bit=0;bit<256;++bit) {
        uint8_t *digest=field==0?provider.compiler.digest:field==1?provider.linker.digest:sdk.identity;
        digest[bit/8]^=(uint8_t)(1u<<(bit%8));
        CHECK(xr_cli_native_probe_check_inputs(&owner,&provider,&sdk)==XR_CLI_NATIVE_PROBE_INTEGRITY);
        digest[bit/8]^=(uint8_t)(1u<<(bit%8));
        CHECK(xr_cli_native_probe_check_inputs(&owner,&provider,&sdk)==XR_CLI_NATIVE_PROBE_OK);
    }
    for (unsigned field=0;field<2;++field) for (unsigned bit=0;bit<64;++bit) {
        uint64_t *length=field?&provider.linker.length:&provider.compiler.length;
        *length^=UINT64_C(1)<<bit;
        CHECK(xr_cli_native_probe_check_inputs(&owner,&provider,&sdk)==XR_CLI_NATIVE_PROBE_INTEGRITY);
        *length^=UINT64_C(1)<<bit;
    }
    XrCompileResourceStats before=rp_stats(&context);owner.report.ready=false;
    CHECK(xr_cli_native_probe_check_inputs(&owner,&provider,&sdk)==XR_CLI_NATIVE_PROBE_INVALID);
    XrCompileResourceStats after=rp_stats(&context);CHECK(!memcmp(&before,&after,sizeof(before)));
    CHECK(rp_live==physical && rp_live_bytes==bytes);native_probe_balanced(&context,baseline);
    uint64_t exact=1+6+2*sizeof(uint64_t)+3*32;
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;limits.work=exact-minus;
        context=native_probe_context(limits);baseline=rp_stats(&context).live_bytes;
        owner.resources=context.resources;owner.report.ready=true;
        CHECK(xr_cli_native_probe_check_inputs(&owner,&provider,&sdk)==(minus?XR_CLI_NATIVE_PROBE_BUDGET:XR_CLI_NATIVE_PROBE_OK));
        native_probe_balanced(&context,baseline);
    }
}
static void native_probe_resolution(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};
    XrXirCompileContext context=native_probe_context(caps);uint64_t baseline=rp_stats(&context).live_bytes;
    const XrCliToolchainPreference preferences[]={
        {"x86_64-linux-gnu",NULL,NULL,XR_TOOLCHAIN_SELECTOR_CLANG},
        {"native","pinned-cl",NULL,XR_TOOLCHAIN_SELECTOR_MSVC}};
    XrCliToolchainConfigView view={0};view.preferences=preferences;view.count=2;view.allow_fallback=true;
    XrCliNativeProbeOptions options={&view,"native",NULL,NULL};XrCliNativeProbeRequest output={0};
    CHECK(xr_cli_native_probe_resolve(context.resources,&options,&output)==XR_CLI_NATIVE_PROBE_OK &&
        output.selector==XR_TOOLCHAIN_SELECTOR_MSVC && !strcmp(output.compiler,"pinned-cl") && output.allow_fallback);
    options.provider="auto";
    CHECK(xr_cli_native_probe_resolve(context.resources,&options,&output)==XR_CLI_NATIVE_PROBE_OK &&
        output.selector==XR_TOOLCHAIN_SELECTOR_AUTO && !strcmp(output.compiler,"pinned-cl"));
    options.provider="clang";
    CHECK(xr_cli_native_probe_resolve(context.resources,&options,&output)==XR_CLI_NATIVE_PROBE_OK &&
        output.selector==XR_TOOLCHAIN_SELECTOR_CLANG && !output.compiler);
    options.provider="auto";options.compiler="explicit";view.allow_fallback=false;
    CHECK(xr_cli_native_probe_resolve(context.resources,&options,&output)==XR_CLI_NATIVE_PROBE_OK &&
        !strcmp(output.compiler,"explicit") && !output.allow_fallback);
    options.provider="invalid";XrCliNativeProbeRequest held;memcpy(&held,&output,sizeof(held));
    CHECK(xr_cli_native_probe_resolve(context.resources,&options,&output)==XR_CLI_NATIVE_PROBE_INVALID &&
        !memcmp(&output,&held,sizeof(output)));
    native_probe_balanced(&context,baseline);
    options.provider="auto";uint64_t exact=1+14+sizeof(output);
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps;limits.work=exact-minus;
        context=native_probe_context(limits);baseline=rp_stats(&context).live_bytes;memcpy(&output,&held,sizeof(output));
        CHECK(xr_cli_native_probe_resolve(context.resources,&options,&output)==
            (minus?XR_CLI_NATIVE_PROBE_BUDGET:XR_CLI_NATIVE_PROBE_OK));
        if (minus) CHECK(!memcmp(&output,&held,sizeof(output)));
        native_probe_balanced(&context,baseline);
    }
}
int main(void) {
    native_probe_owner_failures();native_probe_input_comparison();native_probe_resolution();
    puts("Native probe owner OOM/three-axis exits and every image/SDK identity bit passed; full probe FI remains separate");
    return 0;
}
