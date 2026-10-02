/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_target.c - Independent provider and sysroot file identities
 *
 * KEY CONCEPT:
 *   The immutable result owns commands and leases, but grants no execution rights.
 */
#include "xtc_xir_sysroot_internal.h"
#include "../../base/xsha256.h"
#include "../../base/xutf8.h"
#include <string.h>

static bool target_environment_key(XrXirTargetSnapshot *snapshot, const char *key) {
    for (size_t i = 0;; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return false;
        unsigned char c = (unsigned char)key[i];
        if (!c) return i != 0 || xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
        if (c < 0x21 || c > 0x7e || c == '=') return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
    }
}
static bool target_same_key(XrXirTargetSnapshot *snapshot, const char *a, const char *b, bool *same) {
    for (;;) {
        if (!xtc_xir_target_work(snapshot, 2)) return false;
        unsigned char left = (unsigned char)*a++, right = (unsigned char)*b++;
        if (left >= 'a' && left <= 'z') left -= 'a' - 'A';
        if (right >= 'a' && right <= 'z') right -= 'a' - 'A';
        if (left != right) { *same = false; return true; }
        if (!left) { *same = true; return true; }
    }
}

static XrXirTargetStatus target_resource_status(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_XIR_TARGET_OK;
    case XR_COMPILE_RESOURCE_BUDGET: return XR_XIR_TARGET_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_XIR_TARGET_OUT_OF_MEMORY;
    default: return XR_XIR_TARGET_INVALID;
    }
}
XR_FUNC bool xtc_xir_target_fail(XrXirTargetSnapshot *snapshot, XrXirTargetStatus status) {
    if (snapshot->status == XR_XIR_TARGET_OK) snapshot->status = status;
    return false;
}
XR_FUNC bool xtc_xir_target_work(XrXirTargetSnapshot *snapshot, uint64_t work) {
    if (snapshot->status != XR_XIR_TARGET_OK) return false;
    XrXirTargetStatus status = target_resource_status(xr_compile_resources_work(snapshot->resources, work));
    return status == XR_XIR_TARGET_OK || xtc_xir_target_fail(snapshot, status);
}
XR_FUNC void *xtc_xir_target_allocate(XrXirTargetSnapshot *snapshot, size_t bytes) {
    if (snapshot->status != XR_XIR_TARGET_OK) return NULL;
    if (bytes > SIZE_MAX - sizeof(XtcXirMemory)) {
        xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET); return NULL;
    }
    void *allocation = NULL;
    XrXirTargetStatus status = target_resource_status(xr_compile_resources_calloc(snapshot->resources,
        1, sizeof(XtcXirMemory) + bytes, &allocation));
    if (status != XR_XIR_TARGET_OK) { xtc_xir_target_fail(snapshot, status); return NULL; }
    XtcXirMemory *memory = allocation;
    memory->next = snapshot->memory; snapshot->memory = memory;
    return memory + 1;
}
XR_FUNC bool xtc_xir_target_length(XrXirTargetSnapshot *snapshot, const char *text, size_t *length) {
    if (!text) return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
    for (size_t i = 0; i < XTC_XIR_TARGET_TEXT_LIMIT; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return false;
        if (!text[i]) { *length = i; return true; }
    }
    return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET);
}
XR_FUNC bool xtc_xir_target_compare(XrXirTargetSnapshot *snapshot, const char *a, const char *b, int *order) {
    for (;;) {
        if (!xtc_xir_target_work(snapshot, 2)) return false;
        unsigned char left = (unsigned char)*a++, right = (unsigned char)*b++;
        if (left != right || !left) { *order = (int)left - (int)right; return true; }
    }
}
XR_FUNC char *xtc_xir_target_text(XrXirTargetSnapshot *snapshot, const char *text) {
    size_t length;
    if (!xtc_xir_target_length(snapshot, text, &length) || !xtc_xir_target_work(snapshot, length)) return NULL;
    if (!xr_utf8_validate(text, length)) { xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID); return NULL; }
    char *copy = xtc_xir_target_allocate(snapshot, length + 1);
    if (copy && xtc_xir_target_work(snapshot, length + 1)) memcpy(copy, text, length + 1);
    else return NULL;
    return copy;
}
static bool target_commands(XrXirTargetSnapshot *snapshot, const XrXirTargetRequest *request) {
    snapshot->commands = xtc_xir_target_allocate(snapshot, request->command_count * sizeof(*snapshot->commands));
    if (!snapshot->commands) return false;
    for (uint32_t i = 0; i < request->command_count; ++i) {
        const XrXirTargetCommand *input = &request->commands[i];
        XrXirTargetCommand *output = &snapshot->commands[i];
        if (!input->argc || !input->argv || input->argc > XTC_XIR_TARGET_ARGUMENT_LIMIT ||
            input->environment_count > XTC_XIR_TARGET_ARGUMENT_LIMIT ||
            (input->environment_count && !input->environment))
            return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
        output->cwd = xtc_xir_target_text(snapshot, input->cwd);
        const char **argv = xtc_xir_target_allocate(snapshot, ((size_t)input->argc + 1) * sizeof(*argv));
        if (!output->cwd || !argv) return false;
        output->argv = argv; output->argc = input->argc;
        for (uint32_t a = 0; a < input->argc; ++a) {
            argv[a] = xtc_xir_target_text(snapshot, input->argv[a]);
            if (!argv[a]) return false;
            if (!a && (!xtc_xir_target_work(snapshot, 1) || !argv[a][0]))
                return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
        }
        if (!input->environment_count) continue;
        XrXirTargetEnvironment *env = xtc_xir_target_allocate(snapshot, input->environment_count * sizeof(*env));
        if (!env) return false;
        output->environment = env; output->environment_count = input->environment_count;
        for (uint32_t e = 0; e < input->environment_count; ++e) {
            env[e].key = xtc_xir_target_text(snapshot, input->environment[e].key);
            env[e].value = xtc_xir_target_text(snapshot, input->environment[e].value);
            if (!env[e].key || !env[e].value) return false;
            if (!target_environment_key(snapshot, env[e].key)) return false;
            for (uint32_t p = 0; p < e; ++p) {
                bool same;
                if (!target_same_key(snapshot, env[e].key, env[p].key, &same)) return false;
                if (same) return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
            }
        }
    }
    return true;
}
static bool target_hash_bytes(XrXirTargetSnapshot *snapshot, XrSHA256Context *hash,
    const void *bytes, size_t length) {
    if (!xtc_xir_target_work(snapshot, length)) return false;
    xr_sha256_update(hash, bytes, length); return true;
}
static bool target_u32(XrXirTargetSnapshot *snapshot, XrSHA256Context *hash, uint32_t value) {
    uint8_t bytes[4];
    if (!xtc_xir_target_work(snapshot, sizeof(bytes))) return false;
    for (uint32_t i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
    return target_hash_bytes(snapshot, hash, bytes, sizeof(bytes));
}
static bool target_u64(XrXirTargetSnapshot *snapshot, XrSHA256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    if (!xtc_xir_target_work(snapshot, sizeof(bytes))) return false;
    for (uint32_t i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
    return target_hash_bytes(snapshot, hash, bytes, sizeof(bytes));
}
static bool target_string(XrXirTargetSnapshot *snapshot, XrSHA256Context *hash, const char *value) {
    size_t length;
    return xtc_xir_target_length(snapshot, value, &length) &&
        target_u32(snapshot, hash, (uint32_t)length) && target_hash_bytes(snapshot, hash, value, length);
}
static bool target_hash_file(XrXirTargetSnapshot *snapshot, XrSHA256Context *hash, const XrXirTargetFile *file) {
    return target_u32(snapshot, hash, file->kind) && target_string(snapshot, hash, file->path) &&
        target_u64(snapshot, hash, file->length) && target_hash_bytes(snapshot, hash, file->digest, sizeof(file->digest));
}
static bool target_kind(uint32_t kind, bool provider) {
    return provider ? kind <= XR_XIR_TARGET_PROVIDER_SUPPORT :
        kind >= XR_XIR_TARGET_HEADER && kind <= XR_XIR_TARGET_SYSTEM_LIBRARY;
}
static bool target_family_identity(XrXirTargetSnapshot *snapshot, bool provider, uint8_t digest[32]) {
    static const char provider_domain[] = "xray:xir-provider-files:v1", sysroot_domain[] = "xray:xir-sysroot-files:v1";
    const char *domain = provider ? provider_domain : sysroot_domain;
    XrSHA256Context hash; xr_sha256_init(&hash);
    if (!target_hash_bytes(snapshot, &hash, domain, provider ? sizeof(provider_domain)-1 : sizeof(sysroot_domain)-1) ||
        !target_u32(snapshot, &hash, provider ? snapshot->facts.provider : snapshot->facts.crt)) return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < snapshot->facts.file_count; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return false;
        if (target_kind(snapshot->files[i].kind, provider)) ++count;
    }
    if (!target_u32(snapshot, &hash, count)) return false;
    for (uint32_t i = 0; i < snapshot->facts.file_count; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return false;
        if (target_kind(snapshot->files[i].kind, provider) && !target_hash_file(snapshot, &hash, &snapshot->files[i])) return false;
    }
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    xr_sha256_final(&hash, digest); return true;
}
static bool target_identity(XrXirTargetSnapshot *snapshot) {
    XrXirTargetFacts *facts = &snapshot->facts;
    if (!target_family_identity(snapshot, true, facts->provider_identity) ||
        !target_family_identity(snapshot, false, facts->sysroot_identity)) return false;
    XrSHA256Context hash; xr_sha256_init(&hash);
    const char domain[] = "xray:xir-target-snapshot:v1";
    if (!target_hash_bytes(snapshot, &hash, domain, sizeof(domain) - 1) ||
        !target_u32(snapshot, &hash, facts->schema) || !target_u32(snapshot, &hash, facts->provider) ||
        !target_u32(snapshot, &hash, facts->crt) || !target_u32(snapshot, &hash, facts->dialect) ||
        !target_string(snapshot, &hash, facts->triple) ||
        !target_hash_bytes(snapshot, &hash, facts->provider_identity, 32) ||
        !target_hash_bytes(snapshot, &hash, facts->sysroot_identity, 32) ||
        !target_u32(snapshot, &hash, facts->command_count)) return false;
    for (uint32_t i = 0; i < facts->command_count; ++i) {
        const XrXirTargetCommand *command = &snapshot->commands[i];
        if (!target_string(snapshot, &hash, command->cwd) || !target_u32(snapshot, &hash, command->argc)) return false;
        for (uint32_t a = 0; a < command->argc; ++a)
            if (!target_string(snapshot, &hash, command->argv[a])) return false;
        if (!target_u32(snapshot, &hash, command->environment_count)) return false;
        for (uint32_t e = 0; e < command->environment_count; ++e) {
            if (!target_string(snapshot, &hash, command->environment[e].key) ||
                !target_string(snapshot, &hash, command->environment[e].value)) return false;
        }
    }
    if (!target_u32(snapshot, &hash, facts->file_count)) return false;
    for (uint32_t i = 0; i < facts->file_count; ++i)
        if (!target_hash_file(snapshot, &hash, &snapshot->files[i])) return false;
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    xr_sha256_final(&hash, facts->identity); return true;
}
XR_FUNC XrXirTargetStatus xtc_xir_target_capture(const XrXirTargetRequest *request, XrXirTargetSnapshot **output) {
    if (!request || !output || *output || !request->resources || !request->triple || !request->files || !request->file_count ||
        !request->commands || !request->command_count) return XR_XIR_TARGET_INVALID;
    if (request->file_count > XTC_XIR_TARGET_FILE_LIMIT || request->command_count > XTC_XIR_TARGET_COMMAND_LIMIT)
        return XR_XIR_TARGET_BUDGET;
    if (request->provider < 1 || request->provider > 4 ||
        request->provider == 2 || (request->crt != 1 && request->crt != 2) || request->dialect != 11)
        return XR_XIR_TARGET_UNSUPPORTED;
    void *allocation = NULL;
    XrXirTargetStatus status = target_resource_status(xr_compile_resources_calloc(request->resources,
        1, sizeof(XrXirTargetSnapshot), &allocation));
    if (status != XR_XIR_TARGET_OK) return status;
    XrXirTargetSnapshot *snapshot = allocation; snapshot->resources = request->resources;
    snapshot->facts = (XrXirTargetFacts){1, request->provider, request->crt, request->dialect,
        request->file_count, request->command_count, NULL, {0}, {0}, {0}};
    snapshot->facts.triple = xtc_xir_target_text(snapshot, request->triple);
    int triple_order = 0;
    if (snapshot->facts.triple && xtc_xir_target_compare(snapshot, snapshot->facts.triple, "x86_64-windows-msvc", &triple_order)) {
        if (triple_order) xtc_xir_target_fail(snapshot, XR_XIR_TARGET_UNSUPPORTED);
        else if (target_commands(snapshot, request) && xtc_xir_sysroot_capture(snapshot, request)) target_identity(snapshot);
    }
    status = snapshot->status;
    if (status != XR_XIR_TARGET_OK) { xtc_xir_target_free(snapshot); return status; }
    *output = snapshot; return XR_XIR_TARGET_OK;
}
XR_FUNC const XrXirTargetFacts *xtc_xir_target_facts(const XrXirTargetSnapshot *snapshot) {
    return snapshot ? &snapshot->facts : NULL;
}
XR_FUNC const XrXirTargetFile *xtc_xir_target_file(const XrXirTargetSnapshot *snapshot, uint32_t index) {
    return snapshot && index < snapshot->facts.file_count ? &snapshot->files[index] : NULL;
}
XR_FUNC const XrXirTargetCommand *xtc_xir_target_command(const XrXirTargetSnapshot *snapshot, uint32_t index) {
    return snapshot && index < snapshot->facts.command_count ? &snapshot->commands[index] : NULL;
}
XR_FUNC void xtc_xir_target_free(XrXirTargetSnapshot *snapshot) {
    if (!snapshot) return;
    xtc_xir_sysroot_close(snapshot);
    while (snapshot->memory) {
        XtcXirMemory *memory = snapshot->memory; snapshot->memory = memory->next;
        xr_compile_resources_free(memory);
    }
    xr_compile_resources_free(snapshot);
}
