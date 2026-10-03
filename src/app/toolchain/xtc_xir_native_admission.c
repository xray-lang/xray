/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_admission.c - Cross-check owners before sealing native bytes
 */
#include "xtc_xir_native_admission.h"
#include "../../toolchain/xr_identity_work.h"
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "../../base/xwindows_utf8.h"
#endif

typedef struct NativeAdmission {
    XrIdentityWork work;
    XtcXirNativeAdmissionDiagnostic diagnostic;
    XtcXirNativeOperation *operation;
    const XrXirNativeProjection *projection;
    const XrXirRuntimeSdk *sdk;
    const XrXirCompileContext *context;
    XrXirInvocationFacts facts;
    XrXirNativeProjectionFacts source;
    XrXirRuntimeSdkFacts runtime;
    XrXirInvocationProviderFacts provider;
    XrProcessView commands[3];
    XrXirInvocationFile output;
    uint32_t *indices, selected;
    uint32_t sdk_words[17];
} NativeAdmission;
static const char admission_triple[] = "x86_64-pc-windows-msvc";
static const char admission_options[] = "xray:xir-native-commands:trusted-local:v1:sha256:";
static bool admission_fail(NativeAdmission *a, XtcXirNativeAdmissionStatus status,
    XtcXirNativeAdmissionDomain domain, int code) {
    if (a->diagnostic.status == XTC_XIR_ADMISSION_OK) {
        a->diagnostic.status = status; a->diagnostic.domain = domain; a->diagnostic.code = code;
    }
    return false;
}
static bool admission_resource(NativeAdmission *a, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    return admission_fail(a, status == XR_COMPILE_RESOURCE_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY : XTC_XIR_ADMISSION_INVALID,
        XTC_XIR_ADMISSION_RESOURCE, status);
}
static bool admission_same(NativeAdmission *a, const uint8_t *left, const uint8_t *right) {
    if (identity_same(&a->work, left, right)) return true;
    return a->work.failed ? false : admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
}
static bool admission_compare(NativeAdmission *a, const char *left, const char *right, int *order) {
    if (!left || !right) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    for (;;) {
        if (!identity_work(&a->work, 2)) return false;
        unsigned char l = (unsigned char)*left++, r = (unsigned char)*right++;
        if (l != r || !l) { *order = l < r ? -1 : l > r ? 1 : 0; return true; }
    }
}
static bool admission_text_same(NativeAdmission *a, const char *left, const char *right) {
    int order;
    return admission_compare(a, left, right, &order) && (!order ||
        admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0));
}
static bool admission_text(NativeAdmission *a, XrSHA256Context *hash, const char *text) {
    if (!text) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    uint32_t length = 0;
    for (;;) {
        if (!identity_work(&a->work, 1)) return false;
        if (!text[length]) break;
        if (length == UINT32_MAX) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
        ++length;
    }
    return identity_integer(&a->work, hash, length, 4) && identity_bytes(&a->work, hash, text, length);
}
static bool admission_sdk_words(NativeAdmission *a, const XrXirRuntimeSdkFacts *sdk, uint32_t words[17]) {
#define WORD(i, field) if (!identity_copy(&a->work, words + (i), &sdk->field, sizeof(uint32_t))) return false
    WORD(0, schema); WORD(1, wire); WORD(2, semantic); WORD(3, value_abi); WORD(4, call_abi);
    WORD(5, program_abi); WORD(6, architecture); WORD(7, object_format); WORD(8, hosted);
    WORD(9, c_dialect); WORD(10, crt); WORD(11, sanitizers); WORD(12, allocator);
    WORD(13, assertions); WORD(14, build_provider); WORD(15, abi_recipe_version); WORD(16, closure_recipe_version);
#undef WORD
    return true;
}
static bool admission_producers(NativeAdmission *a) {
    if (!identity_copy(&a->work, &a->facts, xtc_xir_native_operation_facts(a->operation), sizeof(a->facts)) ||
        !identity_copy(&a->work, &a->source, xr_compile_native_projection_facts(a->projection), sizeof(a->source)) ||
        !identity_copy(&a->work, &a->runtime, xr_xir_runtime_sdk_facts(a->sdk), sizeof(a->runtime)) ||
        !identity_copy(&a->work, &a->provider, xtc_xir_native_operation_provider(a->operation), sizeof(a->provider))) return false;
    if (a->facts.kind != XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS || a->facts.completed_runs != 6 || !a->facts.file_count)
        return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    const XrXirSourceProductFacts *s = &a->source.source, *f = &a->facts.projection.source;
#define SAME_WORD(field) do { if (!identity_work(&a->work, 2 * sizeof(s->field))) return false; \
    if (s->field != f->field) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0); } while (0)
    SAME_WORD(target.architecture); SAME_WORD(target.abi_version); SAME_WORD(entry);
    SAME_WORD(function_count); SAME_WORD(module_count);
#undef SAME_WORD
    if (!admission_same(a, s->source_digest, f->source_digest) ||
        !admission_same(a, s->closed_digest, f->closed_digest) ||
        !admission_same(a, s->lowered_layout_digest, f->lowered_layout_digest) ||
        !admission_same(a, a->source.codegen_policy_id.bytes, a->facts.projection.codegen_policy_id.bytes) ||
        !admission_same(a, a->source.generated_digest.bytes, a->facts.projection.generated_digest.bytes) ||
        !admission_text_same(a, a->source.prefix, a->facts.projection.prefix)) return false;
    uint32_t other[17];
    if (!admission_sdk_words(a, &a->runtime, a->sdk_words) || !admission_sdk_words(a, &a->facts.sdk, other)) return false;
    for (unsigned i = 0; i < 17; ++i) {
        if (!identity_work(&a->work, 2 * sizeof(uint32_t))) return false;
        if (other[i] != a->sdk_words[i]) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    }
    if (!admission_same(a, a->runtime.identity, a->facts.sdk.identity) ||
        !identity_work(&a->work, 2 * (sizeof(uint32_t) + sizeof(uint64_t)))) return false;
    if (a->runtime.file_count != a->facts.sdk.file_count || a->runtime.file_bytes != a->facts.sdk.file_bytes)
        return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    if (!identity_work(&a->work, sizeof(a->sdk_words) + sizeof(s->target))) return false;
    if (s->target.architecture != XR_XIR_ARCH_X86_64 || s->target.abi_version != XR_XIR_VALUE_ABI_VERSION ||
        a->runtime.schema != 2 || a->runtime.wire != XR_XIR_CHECKED_SCHEMA || a->runtime.semantic != XR_XIR_CHECKED_CONTRACT ||
        a->runtime.value_abi != XR_XIR_VALUE_ABI_VERSION || a->runtime.call_abi != XR_XIR_CALL_ABI_VERSION ||
        a->runtime.program_abi != XR_XIR_PROGRAM_ABI_VERSION || a->runtime.architecture != 1 ||
        a->runtime.object_format != 1 || a->runtime.hosted != 1 || a->runtime.c_dialect != 11 || a->runtime.crt != 2 ||
        a->runtime.sanitizers != 0 || a->runtime.allocator != 1 || a->runtime.assertions != 0 ||
        a->runtime.abi_recipe_version != 1 || a->runtime.closure_recipe_version != 1)
        return admission_fail(a, XTC_XIR_ADMISSION_UNSUPPORTED, XTC_XIR_ADMISSION_SDK, 0);
    for (unsigned i = 0; i < 3; ++i) {
        a->diagnostic.stage = (XrXirInvocationStage)i;
        const XrProcessView *view = xtc_xir_native_operation_command(a->operation, (XrXirInvocationStage)i);
        if (!view) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, 0);
        if (!identity_copy(&a->work, &a->commands[i], view, sizeof(*view))) return false;
        if (!view->executable || !view->cwd || !view->argv || !view->env_keys || !view->env_values ||
            !view->argc || view->argc > XTC_PROCESS_MAX_ARGS || view->env_count != 3 ||
            (i < 2 && view->argc != 26 + i) || (i == 2 && view->argc < 15))
            return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, 0);
    }
    return true;
}
static bool admission_row(NativeAdmission *a, uint32_t index, XrXirInvocationFile *row) {
    const XrXirInvocationFile *source = xtc_xir_native_operation_file(a->operation, index);
    if (!source) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, 0);
    return identity_copy(&a->work, row, source, sizeof(*row));
}
static bool admission_selected(XrXirInvocationFileKind kind) {
    return kind == XR_XIR_INVOCATION_HEADER || kind == XR_XIR_INVOCATION_CRT || kind == XR_XIR_INVOCATION_SYSTEM ||
        kind == XR_XIR_INVOCATION_PROVIDER_IMAGE || kind == XR_XIR_INVOCATION_PROVIDER_CONFIG;
}
#ifdef XR_OS_WINDOWS
static bool admission_io(NativeAdmission *a, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    return admission_fail(a, status == XR_OS_IO_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY :
        status == XR_OS_IO_NOT_FOUND ? XTC_XIR_ADMISSION_UNRESOLVED :
        status == XR_OS_IO_IO ? XTC_XIR_ADMISSION_IO : XTC_XIR_ADMISSION_INVALID,
        XTC_XIR_ADMISSION_SDK, status);
}
static bool admission_archives(NativeAdmission *a) {
    static const char *const names[5] = {"lib/xray_compile_resources.lib", "lib/xray_xir_admission.lib",
        "lib/xray_xir_declarations.lib", "lib/xray_xir_scalar.lib", "lib/xray_xir_runtime_host.lib"};
    wchar_t *paths[5] = {0}; bool seen[5] = {0}; bool okay = true;
    XrOsIoPolicy policy = xr_compile_io_policy(a->work.resources);
    for (unsigned i = 0; okay && i < 5; ++i) {
        const char *path = NULL;
        XrXirRuntimeSdkStatus status = xr_xir_runtime_sdk_file(a->sdk, names[i], &path);
        if (status != XR_XIR_SDK_OK) {
            okay = admission_fail(a, status == XR_XIR_SDK_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
                status == XR_XIR_SDK_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY :
                status == XR_XIR_SDK_UNRESOLVED ? XTC_XIR_ADMISSION_UNRESOLVED :
                status == XR_XIR_SDK_IO ? XTC_XIR_ADMISSION_IO : XTC_XIR_ADMISSION_INVALID,
                XTC_XIR_ADMISSION_SDK, status);
        } else okay = admission_io(a, xr_win_utf8_text_owned(&policy, path, &paths[i]));
    }
    for (uint32_t i = 0; okay && i < a->facts.file_count; ++i) {
        XrXirInvocationFile row;
        okay = admission_row(a, i, &row);
        if (!okay || row.kind != XR_XIR_INVOCATION_SDK_ARCHIVE) continue;
        wchar_t *path = NULL; bool found = false;
        /* Both owners already publish slash-separated canonical UTF-8 paths.
         * The SDK manifest spelling may still differ in Unicode case. */
        okay = admission_io(a, xr_win_utf8_text_owned(&policy, row.path, &path));
        for (unsigned j = 0; okay && !found && j < 5; ++j) {
            okay = identity_work(&a->work, 1);
            if (!okay) break;
            int result = CompareStringOrdinal(path, -1, paths[j], -1, TRUE);
            if (!result) {
                DWORD error = GetLastError();
                okay = admission_fail(a, error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ?
                    XTC_XIR_ADMISSION_OUT_OF_MEMORY : XTC_XIR_ADMISSION_IO, XTC_XIR_ADMISSION_SDK, (int)error);
            } else if (result == CSTR_EQUAL) {
                if (seen[j]) okay = admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
                else { seen[j] = true; found = true; }
            }
        }
        xr_compile_resources_free(path);
        if (okay && !found) okay = admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    }
    for (unsigned i = 0; i < 5; ++i) {
        xr_compile_resources_free(paths[i]);
        if (okay && !seen[i]) okay = admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    }
    return okay;
}
#else
static bool admission_archives(NativeAdmission *a) {
    return admission_fail(a, XTC_XIR_ADMISSION_UNSUPPORTED, XTC_XIR_ADMISSION_SELF, 0);
}
#endif
static bool admission_files(NativeAdmission *a) {
    uint32_t counts[3][10];
    bool provider_seen[3] = {false, false, false};
    if (!identity_zero(&a->work, counts, sizeof(counts))) return false;
    if ((uint64_t)a->facts.file_count * sizeof(*a->indices) > SIZE_MAX)
        return admission_fail(a, XTC_XIR_ADMISSION_BUDGET, XTC_XIR_ADMISSION_SELF, 0);
    if (!admission_resource(a, xr_compile_resources_alloc(a->work.resources,
        (size_t)a->facts.file_count * sizeof(*a->indices), (void **)&a->indices))) return false;
    const XrXirNativeProjectionSource *source = xr_compile_native_projection_source(a->projection);
    for (uint32_t i = 0; i < a->facts.file_count; ++i) {
        XrXirInvocationFile row;
        if (!admission_row(a, i, &row)) return false;
        a->diagnostic.stage = row.stage;
        if (row.stage > XR_XIR_INVOCATION_LINK || row.stage < XR_XIR_INVOCATION_GENERATED || !row.path)
            return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
        bool compile = row.stage < XR_XIR_INVOCATION_LINK;
        switch (row.kind) {
            case XR_XIR_INVOCATION_SOURCE: case XR_XIR_INVOCATION_OBJECT: case XR_XIR_INVOCATION_HEADER:
                if (!compile) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
                break;
            case XR_XIR_INVOCATION_OUTPUT: case XR_XIR_INVOCATION_SDK_ARCHIVE:
            case XR_XIR_INVOCATION_CRT: case XR_XIR_INVOCATION_SYSTEM:
                if (compile) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
                break;
            case XR_XIR_INVOCATION_PROVIDER_CONFIG:
                if (row.stage == XR_XIR_INVOCATION_LAUNCHER)
                    return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
                break;
            case XR_XIR_INVOCATION_REPORT: case XR_XIR_INVOCATION_PROVIDER_IMAGE: break;
            default: return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
        }
        if (!identity_work(&a->work, sizeof(uint32_t))) return false;
        ++counts[row.stage][row.kind];
        if (row.kind == XR_XIR_INVOCATION_PROVIDER_IMAGE) {
            const XrXirInvocationProviderImageFacts *provider = row.stage == XR_XIR_INVOCATION_LINK ?
                &a->provider.linker : &a->provider.compiler;
            if (!identity_work(&a->work, 2 * sizeof(uint32_t))) return false;
            uint32_t selected = row.stage == XR_XIR_INVOCATION_LAUNCHER ?
                a->provider.launcher_compiler_image_index : provider->observed_image_index;
            if (counts[row.stage][row.kind] - 1 == selected) {
                if (!admission_text_same(a, row.path, provider->path) ||
                    !admission_same(a, row.digest, provider->digest) ||
                    !identity_work(&a->work, sizeof(provider->length))) return false;
                if (row.length != provider->length)
                    return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
                provider_seen[row.stage] = true;
            }
        }
        if (row.kind == XR_XIR_INVOCATION_SOURCE && row.stage == XR_XIR_INVOCATION_GENERATED) {
            if (!identity_work(&a->work, sizeof(source->length)) ||
                !admission_same(a, row.digest, a->source.generated_digest.bytes)) return false;
            if (row.length != source->length) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
        }
        if (row.kind == XR_XIR_INVOCATION_OUTPUT && !identity_copy(&a->work, &a->output, &row, sizeof(row))) return false;
        if (admission_selected(row.kind)) {
            if (!identity_copy(&a->work, &a->indices[a->selected], &i, sizeof(i))) return false;
            ++a->selected;
        }
    }
    if (!identity_work(&a->work, sizeof(counts))) return false;
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!provider_seen[stage] || counts[stage][XR_XIR_INVOCATION_REPORT] != 1 ||
            (stage < 2 && (counts[stage][XR_XIR_INVOCATION_SOURCE] != 1 || counts[stage][XR_XIR_INVOCATION_OBJECT] != 1)) ||
            (stage != 1 && counts[stage][XR_XIR_INVOCATION_PROVIDER_CONFIG] != 1))
            return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    }
    if (counts[2][XR_XIR_INVOCATION_OUTPUT] != 1 || counts[2][XR_XIR_INVOCATION_SDK_ARCHIVE] != 5 ||
        !counts[2][XR_XIR_INVOCATION_CRT] || !counts[2][XR_XIR_INVOCATION_SYSTEM] || !a->selected)
        return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    return admission_archives(a);
}
static bool admission_order(NativeAdmission *a, uint32_t left, uint32_t right, int *order) {
    XrXirInvocationFile l, r;
    if (!admission_row(a, left, &l) || !admission_row(a, right, &r)) return false;
    if (l.stage != r.stage) { *order = l.stage < r.stage ? -1 : 1; return true; }
    if (l.kind != r.kind) { *order = l.kind < r.kind ? -1 : 1; return true; }
    return admission_compare(a, l.path, r.path, order);
}
static bool admission_swap(NativeAdmission *a, uint32_t left, uint32_t right) {
    if (!identity_work(&a->work, 3 * sizeof(uint32_t))) return false;
    uint32_t saved = a->indices[left]; a->indices[left] = a->indices[right]; a->indices[right] = saved; return true;
}
static bool admission_sift(NativeAdmission *a, uint32_t root, uint32_t count) {
    while (root < count / 2) {
        uint32_t child = root * 2 + 1; int order;
        if (!identity_work(&a->work, 2 * sizeof(uint32_t))) return false;
        if (child + 1 < count) {
            if (!admission_order(a, a->indices[child], a->indices[child + 1], &order)) return false;
            if (order < 0) ++child;
        }
        if (!identity_work(&a->work, 2 * sizeof(uint32_t)) ||
            !admission_order(a, a->indices[root], a->indices[child], &order)) return false;
        if (order >= 0) break;
        if (!admission_swap(a, root, child)) return false;
        root = child;
    }
    return true;
}
static bool admission_sysroot(NativeAdmission *a, XrFingerprint *output) {
    static const char domain[] = "xray:xir-native-observed-inputs:trusted-local:v1";
    for (uint32_t i = a->selected / 2; i; --i) if (!admission_sift(a, i - 1, a->selected)) return false;
    for (uint32_t i = a->selected; i > 1; --i)
        if (!admission_swap(a, 0, i - 1) || !admission_sift(a, 0, i - 1)) return false;
    XrSHA256Context hash;
    if (!identity_begin(&a->work, &hash) || !identity_bytes(&a->work, &hash, domain, sizeof(domain) - 1) ||
        !identity_integer(&a->work, &hash, 1, 4) || !identity_integer(&a->work, &hash, a->selected, 4)) return false;
    for (uint32_t i = 0; i < a->selected; ++i) {
        XrXirInvocationFile row;
        if (!identity_work(&a->work, sizeof(uint32_t)) || !admission_row(a, a->indices[i], &row)) return false;
        if (i) {
            int order;
            if (!identity_work(&a->work, 2 * sizeof(uint32_t)) ||
                !admission_order(a, a->indices[i - 1], a->indices[i], &order)) return false;
            if (!order) {
                XrXirInvocationFile prior;
                if (!admission_row(a, a->indices[i - 1], &prior) || !admission_same(a, row.digest, prior.digest)) return false;
                if (row.length != prior.length) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
            }
        }
        if (!identity_integer(&a->work, &hash, row.stage, 4) || !identity_integer(&a->work, &hash, row.kind, 4) ||
            !admission_text(a, &hash, row.path) || !identity_integer(&a->work, &hash, row.length, 8) ||
            !identity_bytes(&a->work, &hash, row.digest, 32)) return false;
    }
    return identity_end(&a->work, &hash, output->bytes);
}
static bool admission_commands(NativeAdmission *a, XrFingerprint *output) {
    static const char domain[] = "xray:xir-native-commands:trusted-local:v1";
    XrSHA256Context hash;
    if (!identity_begin(&a->work, &hash) || !identity_bytes(&a->work, &hash, domain, sizeof(domain) - 1) ||
        !identity_integer(&a->work, &hash, 1, 4) || !identity_integer(&a->work, &hash, 3, 4)) return false;
    for (unsigned stage = 0; stage < 3; ++stage) {
        const XrProcessView *c = &a->commands[stage];
        if (!identity_integer(&a->work, &hash, stage, 4) || !admission_text(a, &hash, c->executable) ||
            !admission_text(a, &hash, c->cwd) || !identity_integer(&a->work, &hash, c->argc, 4)) return false;
        for (size_t i = 0; i < c->argc; ++i)
            if (!identity_work(&a->work, sizeof(char *)) || !admission_text(a, &hash, c->argv[i])) return false;
        if (!identity_integer(&a->work, &hash, c->env_count, 4)) return false;
        for (size_t i = 0; i < c->env_count; ++i)
            if (!identity_work(&a->work, 2 * sizeof(char *)) || !admission_text(a, &hash, c->env_keys[i]) ||
                !admission_text(a, &hash, c->env_values[i])) return false;
        if (!identity_integer(&a->work, &hash, c->timeout_ms, 4) ||
            !identity_integer(&a->work, &hash, c->output_limit, 8) ||
            !identity_integer(&a->work, &hash, c->image_mode, 4)) return false;
    }
    return identity_end(&a->work, &hash, output->bytes);
}
static bool admission_provider(NativeAdmission *a, XrSHA256Context *hash,
    const XrXirInvocationProviderImageFacts *p) {
    if (!admission_text(a, hash, p->path) || !identity_integer(&a->work, hash, p->length, 8) ||
        !identity_bytes(&a->work, hash, p->digest, 32)) return false;
    for (unsigned i = 0; i < 4; ++i) if (!identity_integer(&a->work, hash, p->version.file[i], 2)) return false;
    for (unsigned i = 0; i < 4; ++i) if (!identity_integer(&a->work, hash, p->version.product[i], 2)) return false;
    return true;
}
static bool admission_profile(NativeAdmission *a, const XtcXirPeImage *image,
    const XrFingerprint *commands, const XrFingerprint *sysroot, XrFingerprint *output) {
    static const char domain[] = "xray:xir-native-target-profile:trusted-local:v1";
    XrSHA256Context hash;
    if (!identity_begin(&a->work, &hash) || !identity_bytes(&a->work, &hash, domain, sizeof(domain) - 1) ||
        !identity_integer(&a->work, &hash, 1, 4) || !identity_integer(&a->work, &hash, 1, 4) ||
        !identity_integer(&a->work, &hash, XR_TOOLCHAIN_BINDING_PROVIDER_MSVC, 4) ||
        !admission_text(a, &hash, admission_triple) ||
        !identity_integer(&a->work, &hash, a->source.source.target.architecture, 4) ||
        !identity_integer(&a->work, &hash, a->source.source.target.abi_version, 4)) return false;
    static const uint32_t abi[] = {XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT, XR_XIR_VALUE_ABI_VERSION,
        XR_XIR_CALL_ABI_VERSION, XR_XIR_PROGRAM_ABI_VERSION};
    for (unsigned i = 0; i < sizeof(abi) / sizeof(*abi); ++i)
        if (!identity_integer(&a->work, &hash, abi[i], 4)) return false;
    for (unsigned i = 0; i < 17; ++i) if (!identity_integer(&a->work, &hash, a->sdk_words[i], 4)) return false;
#define FIELD(field, width) if (!identity_integer(&a->work, &hash, image->field, width)) return false
    FIELD(machine, 2); FIELD(optional_magic, 2); FIELD(characteristics, 2); FIELD(subsystem, 2);
    FIELD(entry_rva, 4); FIELD(size_of_image, 4); FIELD(size_of_headers, 4); FIELD(entry_section_characteristics, 4);
#undef FIELD
    return admission_provider(a, &hash, &a->provider.compiler) && admission_provider(a, &hash, &a->provider.linker) &&
        identity_bytes(&a->work, &hash, commands->bytes, 32) && identity_bytes(&a->work, &hash, sysroot->bytes, 32) &&
        identity_bytes(&a->work, &hash, a->runtime.identity, 32) && identity_end(&a->work, &hash, output->bytes);
}
static bool admission_xir(NativeAdmission *a, XrXirStatus status) {
    if (status == XR_XIR_OK) return true;
    return admission_fail(a, status == XR_XIR_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
        status == XR_XIR_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY :
        status == XR_XIR_IO ? XTC_XIR_ADMISSION_IO : status == XR_XIR_UNRESOLVED ? XTC_XIR_ADMISSION_UNRESOLVED :
        XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_XIR, status);
}
static bool admission_read(NativeAdmission *a, uint64_t limit, void **bytes, size_t *length) {
    a->diagnostic.stage = XR_XIR_INVOCATION_LINK;
    XtcXirNativeOperationStatus status = xtc_xir_native_operation_read_output(a->operation, limit, bytes, length);
    if (status == XTC_XIR_NATIVE_OK) return true;
    /* Preserve the operation's original typed failure even after its phase changes. */
    a->diagnostic.operation = *xtc_xir_native_operation_diagnostic(a->operation);
    return admission_fail(a, status == XTC_XIR_NATIVE_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
        status == XTC_XIR_NATIVE_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY :
        status == XTC_XIR_NATIVE_UNRESOLVED ? XTC_XIR_ADMISSION_UNRESOLVED :
        status == XTC_XIR_NATIVE_IO ? XTC_XIR_ADMISSION_IO : status == XTC_XIR_NATIVE_UNSUPPORTED ?
        XTC_XIR_ADMISSION_UNSUPPORTED : XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, status);
}
static bool admission_image(NativeAdmission *a, const void *bytes, size_t length, XtcXirPeImage *image) {
    XtcXirPeVersionStatus status = xtc_xir_pe_image_parse(a->work.resources, bytes, length, image);
    if (status != XTC_XIR_PE_VERSION_OK)
        return admission_fail(a, status == XTC_XIR_PE_VERSION_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
            status == XTC_XIR_PE_VERSION_UNSUPPORTED ? XTC_XIR_ADMISSION_UNSUPPORTED : XTC_XIR_ADMISSION_INVALID,
            XTC_XIR_ADMISSION_PE, status);
    if (!identity_work(&a->work, sizeof(*image))) return false;
    if (!(image->characteristics & 2) || (image->characteristics & 0x2000) || image->subsystem != 3 ||
        !(image->entry_section_characteristics & 0x20000000))
        return admission_fail(a, XTC_XIR_ADMISSION_UNSUPPORTED, XTC_XIR_ADMISSION_PE, XTC_XIR_PE_VERSION_UNSUPPORTED);
    return true;
}
static bool admission_binding(NativeAdmission *a, const XtcXirPeImage *image, XrToolchainBinding *binding) {
    XrFingerprint commands, sysroot, profile;
    if (!admission_commands(a, &commands) || !admission_sysroot(a, &sysroot) ||
        !admission_profile(a, image, &commands, &sysroot, &profile)) return false;
    char options[sizeof(admission_options) + 64];
    if (!identity_copy(&a->work, options, admission_options, sizeof(admission_options) - 1)) return false;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        if (!identity_work(&a->work, 5)) return false;
        uint8_t byte = commands.bytes[i];
        options[sizeof(admission_options) - 1 + 2 * i] = hex[byte >> 4];
        options[sizeof(admission_options) + 2 * i] = hex[byte & 15];
    }
    if (!identity_work(&a->work, 1)) return false;
    options[sizeof(options) - 1] = 0;
    XrToolchainInput input;
    if (!identity_zero(&a->work, &input, sizeof(input))) return false;
    input.schema_version = XR_TOOLCHAIN_BINDING_SCHEMA_VERSION; input.provider = XR_TOOLCHAIN_BINDING_PROVIDER_MSVC;
    input.provider_version = a->provider.compiler.version.file_text;
    input.target_triple = admission_triple; input.codegen_options = options;
    if (!identity_copy(&a->work, &input.sysroot_id, &sysroot, sizeof(sysroot)) ||
        !identity_copy(&a->work, &input.target_profile_id, &profile, sizeof(profile)) ||
        !identity_copy(&a->work, input.runtime_sdk_id.bytes, a->runtime.identity, 32)) return false;
    XrToolchainBindingStatus status = xr_compile_toolchain_binding_build(a->work.resources, &input, binding);
    return status == XR_TOOLCHAIN_BINDING_OK || admission_fail(a,
        status == XR_TOOLCHAIN_BINDING_BUDGET ? XTC_XIR_ADMISSION_BUDGET : XTC_XIR_ADMISSION_INVALID,
        XTC_XIR_ADMISSION_BINDING, status);
}
XR_FUNC XtcXirNativeAdmissionStatus xtc_xir_native_admit(XtcXirNativeOperation *operation,
    const XrXirNativeProjection *projection, const XrXirRuntimeSdk *sdk, uint64_t byte_limit,
    XrXirNativeArtifact **output, XtcXirNativeAdmissionDiagnostic *diagnostic) {
    NativeAdmission a = {0};
    a.diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
    a.operation = operation; a.projection = projection; a.sdk = sdk;
    a.context = xr_compile_native_projection_context(projection);
    if (!operation || !a.context || !a.context->resources || !sdk || !byte_limit || !output || *output ||
        xtc_xir_native_operation_phase(operation) != XTC_XIR_NATIVE_READY ||
        xtc_xir_native_operation_resources(operation) != a.context->resources ||
        xr_xir_runtime_sdk_resources(sdk) != a.context->resources ||
        !xtc_xir_native_operation_facts(operation) || !xtc_xir_native_operation_provider(operation) ||
        !xr_compile_native_projection_facts(projection) || !xr_compile_native_projection_source(projection) ||
        !xr_xir_runtime_sdk_facts(sdk)) {
        admission_fail(&a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
        if (diagnostic) *diagnostic = a.diagnostic;
        return a.diagnostic.status;
    }
    a.work.resources = a.context->resources;
    void *bytes = NULL; size_t length = 0; XrXirNativeArtifact *artifact = NULL;
    XtcXirPeImage image; XrToolchainBinding binding; XrXirNativeInput input;
    bool okay = admission_producers(&a) && admission_files(&a) &&
        admission_read(&a, byte_limit, &bytes, &length);
    if (okay && length != a.output.length)
        okay = admission_fail(&a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
    if (okay) okay = admission_image(&a, bytes, length, &image) && admission_binding(&a, &image, &binding) &&
        admission_xir(&a, xr_compile_native_projection_bind(projection, &binding, &input));
    size_t bounded = byte_limit > SIZE_MAX ? SIZE_MAX : (size_t)byte_limit;
    if (okay) okay = admission_xir(&a, xr_compile_native_artifact_seal(a.context, &input, bytes, length, bounded, &artifact));
    if (okay) okay = admission_same(&a, xr_compile_native_artifact_view(artifact)->native_digest.bytes, a.output.digest);
    if (okay) okay = identity_copy(&a.work, output, &artifact, sizeof(artifact));
    if (okay) artifact = NULL;
    xr_compile_native_artifact_free(artifact);
    xr_compile_resources_free(bytes); xr_compile_resources_free(a.indices);
    if (a.work.failed) admission_fail(&a, XTC_XIR_ADMISSION_BUDGET, XTC_XIR_ADMISSION_RESOURCE, XR_COMPILE_RESOURCE_BUDGET);
    if (diagnostic) *diagnostic = a.diagnostic;
    return a.diagnostic.status;
}
