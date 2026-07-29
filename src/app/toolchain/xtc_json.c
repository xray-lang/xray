/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_json.c - Stable machine-readable toolchain protocol v1
 */

#include "xtc_json.h"

static const char *xtc_json_profile_name(XrToolchainProfile profile) {
    return profile == XR_TOOLCHAIN_PROFILE_FREESTANDING ? "freestanding" : "hosted";
}

static const char *xtc_json_capability_name(XrToolchainCapabilityState state) {
    switch (state) {
        case XR_TOOLCHAIN_CAPABILITY_OK:
            return "ok";
        case XR_TOOLCHAIN_CAPABILITY_FAILED:
            return "failed";
        case XR_TOOLCHAIN_CAPABILITY_SKIPPED:
            return "skipped";
        case XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED:
            return "unsupported";
    }
    return "unsupported";
}

XR_FUNC void xtc_json_write_string(FILE *out, const char *text) {
    if (!out)
        return;
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) (text ? text : ""); *p; p++) {
        switch (*p) {
            case '"':
                fputs("\\\"", out);
                break;
            case '\\':
                fputs("\\\\", out);
                break;
            case '\b':
                fputs("\\b", out);
                break;
            case '\f':
                fputs("\\f", out);
                break;
            case '\n':
                fputs("\\n", out);
                break;
            case '\r':
                fputs("\\r", out);
                break;
            case '\t':
                fputs("\\t", out);
                break;
            default:
                if (*p < 0x20)
                    fprintf(out, "\\u%04x", (unsigned int) *p);
                else
                    fputc((int) *p, out);
                break;
        }
    }
    fputc('"', out);
}

XR_FUNC bool xtc_probe_json_write(FILE *out, const char *requested_target,
                                  const XrToolchainProbeOptions *options,
                                  const XrToolchainProbeResult *result, bool ready,
                                  const char *xray_version, const char *xray_build) {
    if (!out || !options || !result)
        return false;
    fputs("{\"schema\":1,\"xray\":{\"version\":", out);
    xtc_json_write_string(out, xray_version);
    fputs(",\"build\":", out);
    xtc_json_write_string(out, xray_build);
    fputs(",\"sdkDigest\":", out);
    xtc_json_write_string(out, result->runtime.sdk_digest);
    fputs("},\"request\":{\"target\":", out);
    xtc_json_write_string(out, requested_target);
    fputs(",\"normalizedTarget\":", out);
    xtc_json_write_string(out, result->selection.target.name);
    fputs(",\"profile\":", out);
    xtc_json_write_string(out, xtc_json_profile_name(options->profile));
    fputs(",\"provider\":", out);
    xtc_json_write_string(out, xtc_selector_name(options->request.selector));
    fputs("},\"selection\":{\"provider\":", out);
    xtc_json_write_string(out, xtc_provider_name(result->selection.provider));
    fputs(",\"ownership\":", out);
    xtc_json_write_string(out, xtc_ownership_name(result->selection.ownership));
    fputs(",\"compiler\":", out);
    xtc_json_write_string(out, result->selection.program);
    fputs(",\"version\":", out);
    xtc_json_write_string(out, result->selection.version);
    fputs(",\"targetAbi\":", out);
    xtc_json_write_string(out, result->selection.target.name);
    fputs(",\"runtimeArtifact\":", out);
    xtc_json_write_string(out, result->selection.runtime_artifact);
    fprintf(out, ",\"ready\":%s,\"fallbackUsed\":%s},\"capabilities\":{", ready ? "true" : "false",
            result->selection.fallback_used ? "true" : "false");
    fputs("\"cCompile\":", out);
    xtc_json_write_string(out, xtc_json_capability_name(result->c_compile));
    fputs(",\"sdkCompile\":", out);
    xtc_json_write_string(out, xtc_json_capability_name(result->sdk_compile));
    fputs(",\"runtimeLink\":", out);
    xtc_json_write_string(out, xtc_json_capability_name(result->runtime_link));
    fputs(",\"nativeRun\":", out);
    xtc_json_write_string(out, xtc_json_capability_name(result->native_run));
    fputs(",\"cross\":", out);
    xtc_json_write_string(out, xtc_json_capability_name(result->cross));
    fputs(",\"lto\":", out);
    xtc_json_write_string(out, xtc_json_capability_name(result->lto));
    fputs("},\"diagnostics\":[", out);
    for (size_t i = 0; i < result->diagnostic_count; i++) {
        if (i)
            fputc(',', out);
        fputs("{\"code\":", out);
        xtc_json_write_string(out, xtc_reason_code_name(result->diagnostics[i].code));
        fputs(",\"stage\":", out);
        xtc_json_write_string(out, result->diagnostics[i].stage);
        fputs(",\"message\":", out);
        xtc_json_write_string(out, result->diagnostics[i].message);
        fputc('}', out);
    }
    fputs("],\"probe\":{\"id\":", out);
    xtc_json_write_string(out, result->probe_id);
    fputs(",\"fingerprint\":", out);
    xtc_json_write_string(out, result->selection.probe_fingerprint);
    fputs(",\"cache\":", out);
    xtc_json_write_string(out, result->cache);
    fprintf(out, ",\"durationMs\":%llu}}\n", (unsigned long long) result->duration_ms);
    return ferror(out) == 0;
}
