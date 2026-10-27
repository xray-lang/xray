/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_admission.c - Cross-check owners before sealing native bytes
 */
#include "xtc_xir_native_admission.h"
#include "../../base/xsha256.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "../../base/xwindows_utf8.h"
#endif

typedef struct NativeAdmission {
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
static const char admission_options[] = "xray:xir-native-commands:trusted-local:v2:sha256:";
static bool admission_fail(NativeAdmission *a, XtcXirNativeAdmissionStatus status,
    XtcXirNativeAdmissionDomain domain, int code) {
    if (a->diagnostic.status == XTC_XIR_ADMISSION_OK) {
        a->diagnostic.status = status; a->diagnostic.domain = domain; a->diagnostic.code = code;
    }
    return false;
}
static bool admission_resource(NativeAdmission *a, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    return admission_fail(a, status == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XTC_XIR_ADMISSION_MEMORY_LIMIT :
        status == XR_COMPILE_RESOURCE_OVERFLOW ? XTC_XIR_ADMISSION_OVERFLOW :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY : XTC_XIR_ADMISSION_INVALID,
        XTC_XIR_ADMISSION_RESOURCE, status);
}
static bool admission_same(NativeAdmission *a, const uint8_t *left, const uint8_t *right) {
    return !memcmp(left, right, 32) ||
        admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
}
static bool admission_compare(NativeAdmission *a, const char *left, const char *right, int *order) {
    if (!left || !right) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    for (;;) {

        unsigned char l = (unsigned char)*left++, r = (unsigned char)*right++;
        if (l != r || !l) { *order = l < r ? -1 : l > r ? 1 : 0; return true; }
    }
}
static bool admission_text_same(NativeAdmission *a, const char *left, const char *right) {
    int order;
    return admission_compare(a, left, right, &order) && (!order ||
        admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0));
}
static void admission_integer(XrSHA256Context *hash, uint64_t value, unsigned count) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < count; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
    xr_sha256_update(hash, bytes, count);
}
static bool admission_text(NativeAdmission *a, XrSHA256Context *hash, const char *text) {
    if (!text) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    uint32_t length = 0;
    for (;;) {

        if (!text[length]) break;
        if (length == UINT32_MAX) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
        ++length;
    }
    admission_integer(hash, length, 4);
    xr_sha256_update(hash, (const uint8_t *)text, length);
    return true;
}
static void admission_sdk_words(const XrXirRuntimeSdkFacts *sdk, uint32_t words[17]) {
#define WORD(i, field) memcpy(words + (i), &sdk->field, sizeof(uint32_t))
    WORD(0, schema); WORD(1, wire); WORD(2, semantic); WORD(3, value_abi); WORD(4, call_abi);
    WORD(5, program_abi); WORD(6, architecture); WORD(7, object_format); WORD(8, hosted);
    WORD(9, c_dialect); WORD(10, crt); WORD(11, sanitizers); WORD(12, allocator);
    WORD(13, assertions); WORD(14, build_provider); WORD(15, abi_recipe_version); WORD(16, closure_recipe_version);
#undef WORD
}
static bool admission_producers(NativeAdmission *a) {
    a->facts = *xtc_xir_native_operation_facts(a->operation);
    a->source = *xr_compile_native_projection_facts(a->projection);
    a->runtime = *xr_xir_runtime_sdk_facts(a->sdk);
    a->provider = *xtc_xir_native_operation_provider(a->operation);
    if (a->facts.kind != XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS || a->facts.completed_runs != 6 || !a->facts.file_count)
        return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SELF, 0);
    const XrXirSourceProductFacts *s = &a->source.source, *f = &a->facts.projection.source;
#define SAME_WORD(field) do { \
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
    admission_sdk_words(&a->runtime, a->sdk_words);
    admission_sdk_words(&a->facts.sdk, other);
    for (unsigned i = 0; i < 17; ++i) {

        if (other[i] != a->sdk_words[i]) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    }
    if (!admission_same(a, a->runtime.identity, a->facts.sdk.identity)) return false;
    if (a->runtime.file_count != a->facts.sdk.file_count || a->runtime.file_bytes != a->facts.sdk.file_bytes)
        return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);

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
        a->commands[i] = *view;
        if (!view->executable || !view->cwd || !view->argv || !view->env_keys || !view->env_values ||
            view->completion_policy != XR_PROC_COMPLETE_ROOT || view->image_mode != XR_PROC_IMAGES_WINDOWS_TREE ||
            !view->argc || view->argc > XTC_PROCESS_MAX_ARGS || view->env_count != 3 ||
            (i < 2 && view->argc != 26 + i) || (i == 2 && view->argc < 15))
            return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, 0);
    }
    return true;
}
static bool admission_row(NativeAdmission *a, uint32_t index, XrXirInvocationFile *row) {
    const XrXirInvocationFile *source = xtc_xir_native_operation_file(a->operation, index);
    if (!source) return admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, 0);
    *row = *source;
    return true;
}
static bool admission_selected(XrXirInvocationFileKind kind) {
    return kind == XR_XIR_INVOCATION_HEADER || kind == XR_XIR_INVOCATION_CRT || kind == XR_XIR_INVOCATION_SYSTEM ||
        kind == XR_XIR_INVOCATION_PROVIDER_IMAGE || kind == XR_XIR_INVOCATION_PROVIDER_CONFIG;
}
#ifdef XR_OS_WINDOWS
static bool admission_io(NativeAdmission *a, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    return admission_fail(a, status == XR_OS_IO_OVERFLOW ? XTC_XIR_ADMISSION_OVERFLOW :
        status == XR_OS_IO_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY :
        status == XR_OS_IO_NOT_FOUND ? XTC_XIR_ADMISSION_UNRESOLVED :
        status == XR_OS_IO_IO ? XTC_XIR_ADMISSION_IO : XTC_XIR_ADMISSION_INVALID,
        XTC_XIR_ADMISSION_SDK, status);
}
static bool admission_archives(NativeAdmission *a) {
    static const char *const names[5] = {"lib/xray_compile_resources.lib", "lib/xray_xir_admission.lib",
        "lib/xray_xir_declarations.lib", "lib/xray_xir_scalar.lib", "lib/xray_xir_runtime_host.lib"};
    wchar_t *paths[5] = {0}; bool seen[5] = {0}; bool okay = true;
    XrOsIoPolicy policy = xr_compile_io_policy(a->context->resources);
    for (unsigned i = 0; okay && i < 5; ++i) {
        const char *path = NULL;
        XrXirRuntimeSdkStatus status = xr_xir_runtime_sdk_file(a->sdk, names[i], &path);
        if (status != XR_XIR_SDK_OK) {
            okay = admission_fail(a, status == XR_XIR_SDK_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
                status == XR_XIR_SDK_OUT_OF_MEMORY ? XTC_XIR_ADMISSION_OUT_OF_MEMORY :
        status == XR_XIR_SDK_MEMORY_LIMIT ? XTC_XIR_ADMISSION_MEMORY_LIMIT :
        status == XR_XIR_SDK_OVERFLOW ? XTC_XIR_ADMISSION_OVERFLOW :
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
    memset(counts, 0, sizeof(counts));
    if ((uint64_t)a->facts.file_count * sizeof(*a->indices) > SIZE_MAX)
        return admission_fail(a, XTC_XIR_ADMISSION_OVERFLOW, XTC_XIR_ADMISSION_SELF, 0);
    if (!admission_resource(a, xr_compile_resources_alloc(a->context->resources,
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

        ++counts[row.stage][row.kind];
        if (row.kind == XR_XIR_INVOCATION_PROVIDER_IMAGE) {
            const XrXirInvocationProviderImageFacts *provider = row.stage == XR_XIR_INVOCATION_LINK ?
                &a->provider.linker : &a->provider.compiler;

            uint32_t selected = row.stage == XR_XIR_INVOCATION_LAUNCHER ?
                a->provider.launcher_compiler_image_index : provider->observed_image_index;
            if (counts[row.stage][row.kind] - 1 == selected) {
                if (!admission_text_same(a, row.path, provider->path) ||
                    !admission_same(a, row.digest, provider->digest)) return false;
                if (row.length != provider->length)
                    return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
                provider_seen[row.stage] = true;
            }
        }
        if (row.kind == XR_XIR_INVOCATION_SOURCE && row.stage == XR_XIR_INVOCATION_GENERATED) {
            if (!admission_same(a, row.digest, a->source.generated_digest.bytes)) return false;
            if (row.length != source->length) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
        }
        if (row.kind == XR_XIR_INVOCATION_OUTPUT) a->output = row;
        if (admission_selected(row.kind)) {
            a->indices[a->selected] = i;
            ++a->selected;
        }
    }

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
static void admission_swap(NativeAdmission *a, uint32_t left, uint32_t right) {

    uint32_t saved = a->indices[left]; a->indices[left] = a->indices[right]; a->indices[right] = saved;
}
static bool admission_sift(NativeAdmission *a, uint32_t root, uint32_t count) {
    while (root < count / 2) {
        uint32_t child = root * 2 + 1; int order;

        if (child + 1 < count) {
            if (!admission_order(a, a->indices[child], a->indices[child + 1], &order)) return false;
            if (order < 0) ++child;
        }
        if (!admission_order(a, a->indices[root], a->indices[child], &order)) return false;
        if (order >= 0) break;
        admission_swap(a, root, child);
        root = child;
    }
    return true;
}
static bool admission_sysroot(NativeAdmission *a, XrFingerprint *output) {
    static const char domain[] = "xray:xir-native-observed-inputs:trusted-local:v1";
    for (uint32_t i = a->selected / 2; i; --i) if (!admission_sift(a, i - 1, a->selected)) return false;
    for (uint32_t i = a->selected; i > 1; --i) {
        admission_swap(a, 0, i - 1);
        if (!admission_sift(a, 0, i - 1)) return false;
    }
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    admission_integer(&hash, 1, 4);
    admission_integer(&hash, a->selected, 4);
    for (uint32_t i = 0; i < a->selected; ++i) {
        XrXirInvocationFile row;
        if (!admission_row(a, a->indices[i], &row)) return false;
        if (i) {
            int order;
            if (!admission_order(a, a->indices[i - 1], a->indices[i], &order)) return false;
            if (!order) {
                XrXirInvocationFile prior;
                if (!admission_row(a, a->indices[i - 1], &prior) || !admission_same(a, row.digest, prior.digest)) return false;
                if (row.length != prior.length) return admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SELF, 0);
            }
        }
        admission_integer(&hash, row.stage, 4);
        admission_integer(&hash, row.kind, 4);
        if (!admission_text(a, &hash, row.path)) return false;
        admission_integer(&hash, row.length, 8);
        xr_sha256_update(&hash, row.digest, 32);
    }
    xr_sha256_final(&hash, output->bytes);
    return true;
}
static bool admission_commands(NativeAdmission *a, XrFingerprint *output) {
    static const char domain[] = "xray:xir-native-commands:trusted-local:v2";
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    admission_integer(&hash, 2, 4);
    admission_integer(&hash, 3, 4);
    for (unsigned stage = 0; stage < 3; ++stage) {
        const XrProcessView *c = &a->commands[stage];
        admission_integer(&hash, stage, 4);
        if (!admission_text(a, &hash, c->executable) || !admission_text(a, &hash, c->cwd)) return false;
        admission_integer(&hash, c->argc, 4);
        for (size_t i = 0; i < c->argc; ++i)
            if (!admission_text(a, &hash, c->argv[i])) return false;
        admission_integer(&hash, c->env_count, 4);
        for (size_t i = 0; i < c->env_count; ++i)
            if (!admission_text(a, &hash, c->env_keys[i]) || !admission_text(a, &hash, c->env_values[i])) return false;
        admission_integer(&hash, c->timeout_ms, 4);
        admission_integer(&hash, c->output_limit, 8);
        admission_integer(&hash, c->image_mode, 4);
        admission_integer(&hash, c->completion_policy, 4);
    }
    xr_sha256_final(&hash, output->bytes);
    return true;
}
static bool admission_provider(NativeAdmission *a, XrSHA256Context *hash,
    const XrXirInvocationProviderImageFacts *p) {
    if (!admission_text(a, hash, p->path)) return false;
    admission_integer(hash, p->length, 8);
    xr_sha256_update(hash, p->digest, 32);
    for (unsigned i = 0; i < 4; ++i) admission_integer(hash, p->version.file[i], 2);
    for (unsigned i = 0; i < 4; ++i) admission_integer(hash, p->version.product[i], 2);
    return true;
}
static bool admission_profile(NativeAdmission *a, const XtcXirPeImage *image,
    const XrFingerprint *commands, const XrFingerprint *sysroot, XrFingerprint *output) {
    static const char domain[] = "xray:xir-native-target-profile:trusted-local:v1";
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    admission_integer(&hash, 1, 4);
    admission_integer(&hash, 1, 4);
    admission_integer(&hash, XR_TOOLCHAIN_BINDING_PROVIDER_MSVC, 4);
    if (!admission_text(a, &hash, admission_triple)) return false;
    admission_integer(&hash, a->source.source.target.architecture, 4);
    admission_integer(&hash, a->source.source.target.abi_version, 4);
    static const uint32_t abi[] = {XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT, XR_XIR_VALUE_ABI_VERSION,
        XR_XIR_CALL_ABI_VERSION, XR_XIR_PROGRAM_ABI_VERSION};
    for (unsigned i = 0; i < sizeof(abi) / sizeof(*abi); ++i)
        admission_integer(&hash, abi[i], 4);
    for (unsigned i = 0; i < 17; ++i) admission_integer(&hash, a->sdk_words[i], 4);
#define FIELD(field, width) admission_integer(&hash, image->field, width)
    FIELD(machine, 2); FIELD(optional_magic, 2); FIELD(characteristics, 2); FIELD(subsystem, 2);
    FIELD(entry_rva, 4); FIELD(size_of_image, 4); FIELD(size_of_headers, 4); FIELD(entry_section_characteristics, 4);
#undef FIELD
    if (!admission_provider(a, &hash, &a->provider.compiler) || !admission_provider(a, &hash, &a->provider.linker)) return false;
    xr_sha256_update(&hash, commands->bytes, 32);
    xr_sha256_update(&hash, sysroot->bytes, 32);
    xr_sha256_update(&hash, a->runtime.identity, 32);
    xr_sha256_final(&hash, output->bytes);
    return true;
}
static bool admission_xir(NativeAdmission *a, XrXirStatus status) {
    if (status == XR_XIR_OK) return true;
    return admission_fail(a, status == XR_XIR_BUDGET ? XTC_XIR_ADMISSION_BUDGET :
        status == XR_XIR_MEMORY_LIMIT ? XTC_XIR_ADMISSION_MEMORY_LIMIT :
        status == XR_XIR_OVERFLOW ? XTC_XIR_ADMISSION_OVERFLOW :
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
        status == XTC_XIR_NATIVE_MEMORY_LIMIT ? XTC_XIR_ADMISSION_MEMORY_LIMIT :
        status == XTC_XIR_NATIVE_OVERFLOW ? XTC_XIR_ADMISSION_OVERFLOW :
        status == XTC_XIR_NATIVE_UNRESOLVED ? XTC_XIR_ADMISSION_UNRESOLVED :
        status == XTC_XIR_NATIVE_IO ? XTC_XIR_ADMISSION_IO : status == XTC_XIR_NATIVE_UNSUPPORTED ?
        XTC_XIR_ADMISSION_UNSUPPORTED : XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_OPERATION, status);
}
static bool admission_image(NativeAdmission *a, const void *bytes, size_t length, XtcXirPeImage *image) {
    XtcXirPeVersionStatus status = xtc_xir_pe_image_parse(bytes, length, image);
    if (status != XTC_XIR_PE_VERSION_OK)
        return admission_fail(a, status == XTC_XIR_PE_VERSION_COMPLEXITY_LIMIT ? XTC_XIR_ADMISSION_BUDGET :
            status == XTC_XIR_PE_VERSION_UNSUPPORTED ? XTC_XIR_ADMISSION_UNSUPPORTED : XTC_XIR_ADMISSION_INVALID,
            XTC_XIR_ADMISSION_PE, status);

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
    memcpy(options, admission_options, sizeof(admission_options) - 1);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {

        uint8_t byte = commands.bytes[i];
        options[sizeof(admission_options) - 1 + 2 * i] = hex[byte >> 4];
        options[sizeof(admission_options) + 2 * i] = hex[byte & 15];
    }

    options[sizeof(options) - 1] = 0;
    XrToolchainInput input;
    memset(&input, 0, sizeof(input));
    input.schema_version = XR_TOOLCHAIN_BINDING_SCHEMA_VERSION; input.provider = XR_TOOLCHAIN_BINDING_PROVIDER_MSVC;
    input.provider_version = a->provider.compiler.version.file_text;
    input.target_triple = admission_triple; input.codegen_options = options;
    input.sysroot_id = sysroot;
    input.target_profile_id = profile;
    memcpy(input.runtime_sdk_id.bytes, a->runtime.identity, 32);
    XrToolchainBindingStatus status = xr_compile_toolchain_binding_build(&input, binding);
    return status == XR_TOOLCHAIN_BINDING_OK ||
        admission_fail(a, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_BINDING, status);
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
    if (okay) *output = artifact;
    if (okay) artifact = NULL;
    xr_compile_native_artifact_free(artifact);
    xr_compile_resources_free(bytes); xr_compile_resources_free(a.indices);
    if (diagnostic) *diagnostic = a.diagnostic;
    return a.diagnostic.status;
}
