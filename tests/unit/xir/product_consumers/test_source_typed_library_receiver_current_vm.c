/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_typed_library_receiver_current_vm.c - Owned Library receiver suspension
 *
 * KEY CONCEPT:
 *   An authentic Library packet supplies import after its owned Source is removed.
 *   The original receiver survives two real yields and returns a full owned I64.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
#include "base/xsha256.h"
#include "base/xio_policy.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
/* Immutable producer observers retain their fatal invariant checks. */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#undef CHECK
static jmp_buf receiver_failure;
static bool receiver_cleanup_enabled, receiver_observer_failed, receiver_cleanup_failed;
static int receiver_failure_line;
typedef struct ReceiverFailureRecord {
    const char *domain, *operation, *expression;
    int expected, actual, line;
    bool has_status;
} ReceiverFailureRecord;
static ReceiverFailureRecord receiver_first_failure;
static const char *receiver_operation = "bootstrap";
static void record_failure(const char *domain, const char *operation, bool has_status,
    int expected, int actual, const char *expression) {
    int line = receiver_failure_line;
    if (receiver_first_failure.domain) {
        if (!receiver_first_failure.line) receiver_first_failure.line = line;
        return;
    }
    receiver_first_failure = (ReceiverFailureRecord){domain, operation, expression, expected, actual, line, has_status};
}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); \
    if (!receiver_failure_line) receiver_failure_line = __LINE__; \
    record_failure("CONSUMER_OBSERVATION_ASSERTION", receiver_operation, false, 0, 0, #c); \
    if (receiver_cleanup_enabled) { longjmp(receiver_failure, 1); } exit(1); } } while (0)
_Static_assert(XR_XIR_CHECKED_SCHEMA == 27u && XR_XIR_CHECKED_CONTRACT == 72u, "Formal Checked identity");
_Static_assert(XR_XIR_LIBRARY_C_INTERFACE_VERSION == 2u, "Actual owned Catalog interface");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Formal public runtime identity");

typedef struct ReceiverRoles {
    uint32_t entry, answer, private_answer, child, constructor, nominal, library_module;
} ReceiverRoles;
typedef struct ReceiverRun {
    const XrXirCompileContext *context;
    XrCompilerSession *library_session, *program_session;
    XrXirSourceResult library_source;
    XrXirSourceDiagnostic library_diagnostic;
    char *library_failure_path;
    XrXirCheckedPacket library_packet, expected_source, expected_closed;
    XrXirLibraryModuleInput *bindings;
    size_t binding_count;
    XrXirLibraryCatalog *catalog;
    XrXirSourceProduct *product;
    XrCompilerSession *control_session;
    XrXirSourceProduct *control_product;
    XrXirSourceProductDiagnostic control_diagnostic;
    XrModuleResolver *resolver;
    XrModuleId resolved;
    char *resolver_error;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirArtifact *source_checked, *closed, *lowered;
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirValue value;
    struct ReceiverVmOwner *pending_vm_owner;
    uint8_t *transient_bytes;
    size_t transient_size;
    char *transient_canonical, *transient_logical;
    XrXirCheckedPacket transient_packet;
    ReceiverRoles roles;
    char root[XR_PATH_MAX], paths[2][XR_PATH_MAX], stdlib[XR_PATH_MAX];
    bool paths_valid, created[2], library_removed;
    XrXirStatus status;
    const char *operation;
} ReceiverRun;

static void poison(void *pointer, size_t length) {
    volatile uint8_t *bytes = pointer;
    for (size_t i = 0; i < length; ++i) bytes[i] = 0xa5;
}
static void *owned_allocate(const XrXirCompileContext *context, size_t count, size_t stride) {
    void *pointer = NULL;
    XrCompileResourceStatus status = xr_compile_resources_calloc(context->resources, count, stride, &pointer);
    if (status != XR_COMPILE_RESOURCE_OK)
        record_failure("COMPILE_RESOURCE_STATUS", receiver_operation, true, XR_COMPILE_RESOURCE_OK, status, NULL);
    CHECK(status == XR_COMPILE_RESOURCE_OK);
    return pointer;
}
static char *owned_text(const XrXirCompileContext *context, const char *text, size_t length) {
    CHECK(length < XR_PATH_MAX);
    char *copy = owned_allocate(context, length + 1, 1);
    memcpy(copy, text, length); return copy;
}
static void free_text(const char *text) {
    if (!text) return;
    poison((void *)text, strlen(text) + 1); xr_compile_resources_free((void *)text);
}
static void free_poisoned_packet(XrXirCheckedPacket *packet) {
    if (packet->bytes) poison(packet->bytes, packet->length);
    xr_xir_compile_checked_packet_free(packet);
}
static bool completed(ReceiverRun *run, XrXirStatus status, const char *operation) {
    run->operation = operation; run->status = status; receiver_operation = operation;
    if (status == XR_XIR_OK) return true;
    record_failure("PRODUCT_XIR_STATUS", operation, true, XR_XIR_OK, status, NULL);
    fprintf(stderr, "typed-library-receiver operation=%s required=0 actual=%u\n",
        operation, (unsigned)status);
    return false;
}
static bool compile_expected(ReceiverRun *run, XrXirStatus status, XrXirStatus expected, const char *operation) {
    run->operation = operation; run->status = status; receiver_operation = operation;
    if (status == expected) return true;
    record_failure("PRODUCT_XIR_STATUS", operation, true, expected, status, NULL); return false;
}
static bool call_completed(ReceiverRun *run, XrXirCallStatus actual, XrXirCallStatus expected, const char *operation) {
    run->operation = operation; receiver_operation = operation;
    if (actual == expected) return true;
    record_failure("PRODUCT_CALL_STATUS", operation, true, expected, actual, NULL); return false;
}
static bool module_completed(XrModuleStatus actual, const char *operation) {
    receiver_operation = operation;
    if (actual == XR_MODULE_OK) return true;
    record_failure("PRODUCT_MODULE_STATUS", operation, true, XR_MODULE_OK, actual, NULL); return false;
}
static bool session_completed(XrCompilerSessionStatus actual, const char *operation) {
    receiver_operation = operation;
    if (actual == XR_COMPILER_SESSION_OK) return true;
    record_failure("PRODUCT_SESSION_STATUS", operation, true, XR_COMPILER_SESSION_OK, actual, NULL); return false;
}
static bool same_path(const char *left, const char *right) {
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#ifdef XR_OS_WINDOWS
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 'a' - 'A');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 'a' - 'A');
#endif
        if (a != b) return false;
    }
    return !*left && !*right;
}
static void owned_root_check(const ReceiverRun *run, const XrOsIoPolicy *policy) {
    char current[XR_PATH_MAX]; XrFsStat stat = {0};
    CHECK(run->paths_valid);
    CHECK(xr_os_io_stat(policy, run->root, &stat) == XR_OS_IO_OK && stat.kind == XR_FS_DIR);
    CHECK(xr_os_io_realpath(policy, run->root, current, sizeof(current)) == XR_OS_IO_OK);
    CHECK(same_path(current, run->root));
}
static void absent_owned_leaf(const ReceiverRun *run, unsigned leaf) {
    XrOsIoPolicy policy = xr_compile_io_policy(run->context->resources);
    owned_root_check(run, &policy);
    XrFsStat stat; uint8_t unchanged[sizeof(stat)];
    memset(&stat, 0xa5, sizeof(stat)); memcpy(unchanged, &stat, sizeof(stat));
    CHECK(xr_os_io_stat(&policy, run->paths[leaf], &stat) == XR_OS_IO_NOT_FOUND);
    CHECK(!memcmp(&stat, unchanged, sizeof(stat)));
    uint8_t *bytes = NULL; size_t length = 0;
    CHECK(xr_os_io_read_regular_file(&policy, run->paths[leaf], 4096, &bytes, &length) == XR_OS_IO_NOT_FOUND);
    CHECK(!bytes && !length);
}
static void exact_digest(const void *bytes, size_t length, const char *expected) {
    uint8_t sha[32]; char hex[65]; xr_sha256(bytes, length, sha);
    for (unsigned i = 0; i < 32; ++i) {
        hex[2 * i] = "0123456789abcdef"[sha[i] >> 4];
        hex[2 * i + 1] = "0123456789abcdef"[sha[i] & 15];
    }
    hex[64] = 0; CHECK(!strcmp(hex, expected)); poison(sha, sizeof(sha));
}
static void copy_owned_sources(ReceiverRun *run, char **argv) {
    receiver_operation = run->operation = "own-source-copy-and-CREATE_NEW";
    XrOsIoPolicy policy = xr_compile_io_policy(run->context->resources);
    char cwd[XR_PATH_MAX], build[XR_PATH_MAX], expected[XR_PATH_MAX];
    CHECK(xr_os_io_getcwd(&policy, cwd, sizeof(cwd)) == XR_OS_IO_OK);
    CHECK(xr_os_io_realpath(&policy, cwd, build, sizeof(build)) == XR_OS_IO_OK);
    int size = snprintf(expected, sizeof(expected), "%s/generated/current-typed-library-receiver-r1/private-source", build);
    CHECK(size > 0 && (size_t)size < sizeof(expected));
    CHECK(xr_os_io_realpath(&policy, argv[1], run->root, sizeof(run->root)) == XR_OS_IO_OK);
    CHECK(same_path(expected, run->root));
    CHECK(strlen(argv[3]) < sizeof(run->stdlib)); strcpy(run->stdlib, argv[3]);
    const char *leaves[2] = {"root.xr", "library.xr"};
    const size_t lengths[2] = {163, 185};
    const char *digests[2] = {"3bb17ffe354b537a024dbf6e4c371df1c80051d72a3db21f20d48fdbeeb5f8bc",
        "bb41f3d3d610b67ce6445475deb031967b10a0b069ef00b1c923599b3d6a6c87"};
    for (unsigned leaf = 0; leaf < 2; ++leaf) {
        size = snprintf(run->paths[leaf], sizeof(run->paths[leaf]), "%s/%s", run->root, leaves[leaf]);
        CHECK(size > 0 && (size_t)size < sizeof(run->paths[leaf]));
    }
    run->paths_valid = true; owned_root_check(run, &policy);
    for (unsigned leaf = 0; leaf < 2; ++leaf) {
        absent_owned_leaf(run, leaf);
        char fixture[XR_PATH_MAX];
        size = snprintf(fixture, sizeof(fixture), "%s/%s", argv[2], leaves[leaf]);
        CHECK(size > 0 && (size_t)size < sizeof(fixture));
        CHECK(xr_os_io_read_regular_file(&policy, fixture, lengths[leaf], &run->transient_bytes, &run->transient_size) == XR_OS_IO_OK);
        CHECK(run->transient_bytes && run->transient_size == lengths[leaf]);
        exact_digest(run->transient_bytes, run->transient_size, digests[leaf]);
        if (!leaf) exact_digest(run->transient_bytes, 109, "5765d2986533f38100664640b32772722b00c0f32db226ec3f8267d19c687b43");
        CHECK(xr_os_io_write_new_file_sync(&policy, run->paths[leaf], run->transient_bytes, run->transient_size) == XR_OS_IO_OK);
        run->created[leaf] = true;
        poison(run->transient_bytes, run->transient_size); policy.free(policy.context, run->transient_bytes);
        run->transient_bytes = NULL; run->transient_size = 0;
    }
}
static void remove_created_leaf(ReceiverRun *run, unsigned leaf) {
    CHECK(run->created[leaf] && run->paths_valid);
    XrOsIoPolicy policy = xr_compile_io_policy(run->context->resources);
    owned_root_check(run, &policy);
    CHECK(xr_os_io_remove(&policy, run->paths[leaf]) == XR_OS_IO_OK);
    run->created[leaf] = false; absent_owned_leaf(run, leaf);
}
static void construction_owner(ReceiverRun *run, const XrXirArtifact *artifact, const char *operation) {
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    const XrXirConstruction *construction = xr_xir_compile_artifact_construction(artifact);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    CHECK(context && context->resources == run->context->resources && module && construction);
    const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
    uint32_t count = table && table->declarations ? table->count : 0;
    CHECK(xr_xir_compile_construction_count(construction) == count);
    for (uint32_t n = 0; n < count; ++n) {
        const XrXirConstructionRow *row = xr_xir_compile_construction_row(construction, n);
        CHECK(row && row->field_count == table->declarations[n].field_count);
        CHECK(!!row->field_initializers == !!row->field_count);
    }
    CHECK(!xr_xir_compile_construction_row(construction, count));
    CHECK(completed(run, xr_xir_compile_verify_v2(context, module, construction, NULL), operation));
}
static bool literal_is(XrXirLiteral text, const char *expected) {
    size_t length = strlen(expected);
    return text.length == length && !memcmp(text.bytes, expected, length);
}
static bool function_is(const XrXirFunction *function, const char *name) {
    size_t length = strlen(name);
    return function->name_length == length && !memcmp(function->name, name, length);
}
/* This observes the original fixture; only public verification grants validity. */
static ReceiverRoles receiver_shape(const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    const XrXirNominalTable *table = module && module->types ? module->types->nominals : NULL;
    CHECK(d && table && (table->declarations || table->identities));
    ReceiverRoles roles = {d->entry_function, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (uint32_t n = 0; n < table->count; ++n) {
        XrXirLiteral name = table->declarations ? table->declarations[n].name : table->identities[n].name;
        if (!literal_is(name, "Worker")) continue;
        CHECK(roles.nominal == UINT32_MAX); roles.nominal = n;
        uint32_t kind = table->declarations ? table->declarations[n].kind : table->identities[n].kind;
        uint32_t flags = table->declarations ? table->declarations[n].flags : table->identities[n].flags;
        uint32_t exported = table->declarations ? table->declarations[n].exported : table->identities[n].exported;
        uint32_t fields = table->declarations ? table->declarations[n].field_count : table->identities[n].field_count;
        CHECK(kind == XR_XIR_NOMINAL_CLASS && (flags & XR_XIR_NOMINAL_FINAL) && exported && fields == 1);
        XrXirLiteral field = table->declarations ? table->declarations[n].fields[0].name : table->identities[n].fields[0].name;
        CHECK(literal_is(field, "value"));
        if (table->declarations) CHECK(table->declarations[n].fields[0].type == XR_XIR_I64);
    }
    CHECK(roles.nominal != UINT32_MAX);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->nominal_owner == roles.nominal + 1 && identity->method_kind == XR_XIR_CONSTRUCTOR) {
            CHECK(roles.constructor == UINT32_MAX); roles.constructor = f;
        }
        if (identity->nominal_owner == roles.nominal + 1 && function_is(fn, "child")) {
            CHECK(roles.child == UINT32_MAX && identity->method_kind == XR_XIR_READ_METHOD);
            CHECK(fn->parameter_count == 2 && fn->parameters[1] == XR_XIR_I64 && fn->result == XR_XIR_I64);
            roles.child = f; roles.library_module = identity->module;
            unsigned yields = 0, field_reads = 0, additions = 0;
            for (uint32_t i = 0; i < fn->instruction_count; ++i) {
                if (fn->instructions[i].op == XR_XIR_SUSPEND) ++yields;
                if (fn->instructions[i].op == XR_XIR_CLASS_GET) ++field_reads;
                if (fn->instructions[i].op == XR_XIR_ADD_INT) ++additions;
            }
            CHECK(yields == 2 && field_reads == 1 && additions == 1);
        }
        if (module->linkage_kind != XR_XIR_PROGRAM) continue;
        if (identity->module == d->root_module && function_is(fn, "consumerAnswer")) {
            CHECK(roles.answer == UINT32_MAX && identity->exported && !fn->parameter_count && fn->result == XR_XIR_I64);
            roles.answer = f;
        }
        if (identity->module == d->root_module && function_is(fn, "answer")) {
            CHECK(roles.private_answer == UINT32_MAX && !identity->exported && !fn->parameter_count && fn->result == XR_XIR_I64);
            roles.private_answer = f;
        }
    }
    CHECK(roles.child != UINT32_MAX && roles.constructor != UINT32_MAX && roles.library_module < d->module_count);
    if (module->linkage_kind == XR_XIR_PROGRAM) {
        CHECK(roles.entry < module->function_count && roles.answer != UINT32_MAX && roles.private_answer != UINT32_MAX);
        CHECK(roles.library_module != d->root_module && xr_xir_module_imports(d, d->root_module, roles.library_module));
        CHECK(!xr_xir_module_imports(d, roles.library_module, d->root_module));
    } else CHECK(module->linkage_kind == XR_XIR_LIBRARY && roles.entry == UINT32_MAX);
    return roles;
}

/* Enumerate actual Checked ordinals. Unknown authorities or absent paths fail closed. */
static void library_bindings(ReceiverRun *run) {
    receiver_operation = run->operation = "Library-actual-ordinal-binding";
    const XrXirModule *module = xr_xir_compile_artifact_module(run->library_source.checked);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(run->library_source.snapshot);
    CHECK(module && module->declarations && view && view->complete);
    run->binding_count = module->declarations->module_count;
    CHECK(run->binding_count && view->module_count == run->binding_count && view->modules);
    run->bindings = owned_allocate(run->context, run->binding_count, sizeof(*run->bindings));
    unsigned original_library_matches = 0;
    for (size_t m = 0; m < run->binding_count; ++m) {
        const XrXirSourceModule *decl = &module->declarations->modules[m];
        const XrXirSourceQueryModule *query = &view->modules[m];
        CHECK(query->identity && query->path && strlen(query->identity) == decl->name_length);
        CHECK(!memcmp(query->identity, decl->name, decl->name_length));
        XrModuleIdentityKind kind;
        CHECK(xr_module_identity_valid(query->identity, &kind));
        XrXirLibraryModuleInput *binding = &run->bindings[m];
        binding->authority.kind = kind;
        if (kind == XR_MODULE_IDENTITY_SCRIPT) {
            binding->authority.physical_root = owned_text(run->context, run->root, strlen(run->root));
        } else {
            CHECK(kind == XR_MODULE_IDENTITY_STDLIB);
            const char *name = NULL; size_t length = 0;
            CHECK(xr_module_identity_stdlib_namespace(query->identity, &name, &length) && name && length);
            binding->authority.namespace_id = owned_text(run->context, name, length);
            binding->authority.physical_root = owned_text(run->context, run->stdlib, strlen(run->stdlib));
        }
        CHECK(module_completed(xr_compile_module_identity_from_source(run->context->resources, &binding->authority,
            query->path, &run->transient_canonical, &run->transient_logical), "module-identity-from-source"));
        CHECK(run->transient_canonical && run->transient_logical && !strcmp(run->transient_canonical, query->identity));
        binding->logical_path = run->transient_logical; run->transient_logical = NULL;
        free_text(run->transient_canonical); run->transient_canonical = NULL;
        if (kind == XR_MODULE_IDENTITY_SCRIPT && !strcmp(binding->logical_path, "library.xr")) {
            CHECK(same_path(query->path, run->paths[1])); ++original_library_matches;
        }
    }
    CHECK(original_library_matches == 1);
}
static void library_inputs_die(ReceiverRun *run) {
    free_poisoned_packet(&run->library_packet);
    for (size_t m = 0; run->bindings && m < run->binding_count; ++m) {
        free_text(run->bindings[m].logical_path);
        free_text(run->bindings[m].authority.namespace_id);
        free_text(run->bindings[m].authority.physical_root);
    }
    if (run->bindings) {
        poison(run->bindings, run->binding_count * sizeof(*run->bindings));
        xr_compile_resources_free(run->bindings); run->bindings = NULL;
    }
    xr_xir_compile_source_result_free(&run->library_source);
    xr_compile_session_free(run->library_session); run->library_session = NULL;
    free_text(run->library_failure_path); run->library_failure_path = NULL;
    poison(&run->library_diagnostic, sizeof(run->library_diagnostic));
}
static bool library_catalog_build(ReceiverRun *run) {
    CHECK(session_completed(xr_compile_session_new(run->context->resources, &run->library_session), "Library-session-new"));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, run->root};
    XrXirSourceRequest request = {run->library_session, run->paths[1], &authority,
        run->context, run->stdlib, NULL, XR_XIR_LIBRARY, NULL};
    if (!completed(run, xr_xir_compile_source_check(&request, &run->library_source,
        &run->library_diagnostic, &run->library_failure_path), "Library-Source-check")) {
        fprintf(stderr, "Library-message=%.192s path=%s\n", run->library_diagnostic.message,
            run->library_failure_path ? run->library_failure_path : "(none)");
        return false;
    }
    CHECK(run->library_source.checked && run->library_source.snapshot);
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->library_source.checked, NULL), "Library-SOURCE-full-artifact-verify"));
    construction_owner(run, run->library_source.checked, "Library-SOURCE-full-v2");
    (void)receiver_shape(run->library_source.checked);
    if (!completed(run, xr_xir_compile_checked_write(run->library_source.checked,
        &run->library_packet, NULL), "Library-SOURCE-full-packet")) return false;
    library_bindings(run);
    XrXirLibraryInput input = {run->library_packet.bytes, run->library_packet.length, {0},
        run->bindings, run->binding_count};
    CHECK(input.packet && input.length); xr_sha256(input.packet, input.length, input.sha256);
    XrXirStatus status = xr_xir_compile_library_catalog_new_v2(run->context, &input, 1, &run->catalog);
    poison(&input, sizeof(input)); poison(&request, sizeof(request)); poison(&authority, sizeof(authority));
    /* A rejected positive is a test failure, never an accepted negative outcome. */
    if (!completed(run, status, "Library-Catalog-EXPECT-OK")) { CHECK(!run->catalog); return false; }
    CHECK(run->catalog && xr_xir_compile_library_catalog_context(run->catalog)->resources == run->context->resources);
    size_t expected_count = run->binding_count;
    library_inputs_die(run); run->binding_count = 0;
    size_t count = 0;
    const XrModuleResourceBinding *resources = xr_xir_compile_library_catalog_resources_v2(run->catalog, &count);
    CHECK(resources && count == expected_count);
    for (size_t m = 0; m < count; ++m) {
        CHECK(resources[m].canonical && resources[m].logical_path && resources[m].checked);
        CHECK(resources[m].checked_module == m);
        CHECK(xr_xir_compile_artifact_context(resources[m].checked)->resources == run->context->resources);
        CHECK(completed(run, xr_xir_compile_artifact_verify(resources[m].checked, NULL), "Catalog-owned-Library-full-verify"));
        construction_owner(run, resources[m].checked, "Catalog-owned-Library-full-v2");
    }
    CHECK(!run->library_source.checked && !run->library_source.snapshot && !run->library_session);
    remove_created_leaf(run, 1); run->library_removed = true;
    puts("Library-inputs-producer-dead=1 owned-library-source-strict-NOT_FOUND=1 Catalog-live=1");
    return true;
}

static void retain_product_packet(const XrXirCompileContext *context,
    const XrXirSourceProductPacketView *view, XrXirCheckedPacket *packet) {
    CHECK(view->bytes && view->length && !packet->bytes && !packet->length);
    packet->bytes = owned_allocate(context, view->length, 1); packet->length = view->length;
    memcpy(packet->bytes, view->bytes, view->length);
}
static void exact_packet(ReceiverRun *run, const XrXirArtifact *artifact, const XrXirCheckedPacket *expected) {
    CHECK(!run->transient_packet.bytes && !run->transient_packet.length);
    CHECK(completed(run, xr_xir_compile_checked_write(artifact, &run->transient_packet, NULL), "owned-exact-packet-rewrite"));
    CHECK(run->transient_packet.length == expected->length &&
        !memcmp(run->transient_packet.bytes, expected->bytes, run->transient_packet.length));
    xr_xir_compile_checked_packet_free(&run->transient_packet);
}
static void catalog_import_controls(ReceiverRun *run) {
    CHECK(run->catalog && run->library_removed); absent_owned_leaf(run, 1);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, run->root};
    receiver_operation = run->operation = "no-Catalog-original-root-missing-Library-control";
    CHECK(session_completed(xr_compile_session_new(run->context->resources, &run->control_session), "no-Catalog-control-session-new"));
    XrXirSourceProductRequest absent = {{run->control_session, run->paths[0], &authority,
        run->context, run->stdlib, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirStatus status = xr_xir_compile_source_product_build(&absent, &run->control_product, &run->control_diagnostic);
    CHECK(compile_expected(run, status, XR_XIR_UNRESOLVED, "no-Catalog-original-root-missing-Library-control"));
    CHECK(!run->control_product && run->control_diagnostic.status == status);
    CHECK(run->control_diagnostic.stage == XR_XIR_SOURCE_PRODUCT_CHECK &&
        run->control_diagnostic.source.status == status && run->control_diagnostic.source.message[0]);
    printf("no-Catalog unchanged-original-root required=UNRESOLVED actual=%u stage=%u message=%.192s\n",
        (unsigned)status, (unsigned)run->control_diagnostic.stage, run->control_diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&run->control_diagnostic);
    xr_compile_session_free(run->control_session); run->control_session = NULL;
    poison(&absent, sizeof(absent)); absent_owned_leaf(run, 1);
    receiver_operation = run->operation = "public-independent-resolver-CheckedLibrary-selection";
    XrModuleResolverConfig config = {run->stdlib, NULL, run->catalog};
    CHECK(module_completed(xr_compile_module_resolver_new(run->context->resources, &config, &run->resolver), "module-resolver-new"));
    CHECK(run->resolver && run->resolver->resources == run->context->resources && run->resolver->config.catalog == run->catalog);
    CHECK(module_completed(xr_compile_module_resolver_resolve(run->resolver, "./library", run->paths[0], &authority,
        &run->resolved, &run->resolver_error), "module-resolver-resolve"));
    CHECK(run->resolved.representation == XR_MODULE_CHECKED_LIBRARY && run->resolved.resource);
    CHECK(run->resolved.logical_path && !strcmp(run->resolved.logical_path, "library.xr"));
    size_t count = 0;
    const XrModuleResourceBinding *resources = xr_xir_compile_library_catalog_resources_v2(run->catalog, &count);
    bool matched = false;
    for (size_t m = 0; m < count; ++m) {
        if (run->resolved.resource != &resources[m]) continue;
        CHECK(!matched && resources[m].checked && resources[m].authority.kind == XR_MODULE_IDENTITY_SCRIPT);
        CHECK(!strcmp(resources[m].logical_path, "library.xr") && !strcmp(resources[m].canonical, run->resolved.canonical));
        CHECK(resources[m].source_locator && same_path(resources[m].source_locator, run->paths[1])); matched = true;
    }
    CHECK(matched);
    xr_compile_module_id_cleanup(&run->resolved);
    free_text(run->resolver_error); run->resolver_error = NULL;
    xr_compile_module_resolver_free(run->resolver); run->resolver = NULL;
    poison(&config, sizeof(config)); poison(&authority, sizeof(authority));
    puts("independent-public-resolver-selected=CHECKED_LIBRARY actual-Catalog-row=1; SourceProduct graph observation remains separate");
}
static bool program_source_build(ReceiverRun *run) {
    CHECK(run->catalog && run->library_removed); absent_owned_leaf(run, 1);
    catalog_import_controls(run);
    CHECK(session_completed(xr_compile_session_new(run->context->resources, &run->program_session), "Program-Catalog-import-session-new"));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, run->root};
    XrXirSourceProductRequest request = {{run->program_session, run->paths[0], &authority,
        run->context, run->stdlib, NULL, XR_XIR_PROGRAM, run->catalog},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    CHECK(request.source.libraries == run->catalog);
    if (!completed(run, xr_xir_compile_source_product_build(&request, &run->product,
        &run->diagnostic), "Program-SourceProduct-Catalog-import")) {
        fprintf(stderr, "Program-stage=%u message=%.192s path=%s\n", (unsigned)run->diagnostic.stage,
            run->diagnostic.source.message, run->diagnostic.source_path ? run->diagnostic.source_path : "(none)");
        return false;
    }
    CHECK(run->product && xr_xir_compile_source_product_context(run->product)->resources == run->context->resources);
    CHECK(completed(run, xr_xir_compile_source_product_verify(run->product, 16777216, NULL), "Program-SourceProduct-full-verify"));
    XrXirSourceDiagnostic admission = {0};
    CHECK(completed(run, xr_xir_compile_source_product_public_admit(run->product, &admission), "Program-SourceProduct-public-admit"));
    XrXirSourceProductPacketView packet = {0};
    CHECK(completed(run, xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_SOURCE, &packet), "Program-SOURCE-authentic-packet"));
    CHECK(completed(run, xr_xir_compile_checked_read(run->context, packet.bytes, packet.length, &run->source_checked, NULL), "Program-SOURCE-owned-Checked-read"));
    retain_product_packet(run->context, &packet, &run->expected_source);
    CHECK(completed(run, xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet), "Program-CLOSED-authentic-packet"));
    retain_product_packet(run->context, &packet, &run->expected_closed);
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(run->product);
    const XrXirSourceDependencies *dependencies = xr_xir_compile_source_product_dependencies(run->product);
    const XrXirSourceView *view = xr_xir_compile_source_product_view(run->product);
    CHECK(facts && dependencies && view && view->complete && dependencies->count == view->module_count);
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->source_checked, NULL), "Program-owned-SOURCE-full-verify"));
    construction_owner(run, run->source_checked, "Program-owned-SOURCE-full-v2"); run->roles = receiver_shape(run->source_checked);
    CHECK(facts->entry < facts->function_count && facts->target.architecture == XR_XIR_ARCH_X86_64 &&
        facts->target.abi_version == XR_XIR_VALUE_ABI_VERSION);
    CHECK(facts->module_count == view->module_count && run->roles.library_module < view->module_count);
    CHECK(view->modules[run->roles.library_module].path && same_path(view->modules[run->roles.library_module].path, run->paths[1]));
    bool library_dependency = false;
    for (uint32_t m = 0; m < dependencies->count; ++m) {
        const XrXirSourceDependency *dependency = &dependencies->entries[m];
        if (dependency->module != run->roles.library_module) continue;
        CHECK(dependency->path && same_path(dependency->path, run->paths[1])); library_dependency = true;
    }
    CHECK(library_dependency); absent_owned_leaf(run, 1);
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_compile_session_free(run->program_session); run->program_session = NULL;
    xr_xir_compile_library_catalog_free(run->catalog); run->catalog = NULL;
    poison(&request, sizeof(request)); poison(&authority, sizeof(authority));
    CHECK(!run->product && !run->program_session && !run->catalog);
    return true;
}
static bool detach_program(ReceiverRun *run) {
    CHECK(!run->product && !run->program_session && !run->catalog);
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->source_checked, NULL), "detached-SOURCE-full-verify"));
    construction_owner(run, run->source_checked, "detached-SOURCE-full-v2"); (void)receiver_shape(run->source_checked);
    exact_packet(run, run->source_checked, &run->expected_source); free_poisoned_packet(&run->expected_source);
    if (!completed(run, xr_xir_compile_specialize(run->source_checked, &run->closed, NULL), "owned-Source-specialize")) return false;
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->closed, NULL), "detached-CLOSED-full-verify"));
    construction_owner(run, run->closed, "detached-CLOSED-full-v2"); (void)receiver_shape(run->closed);
    exact_packet(run, run->closed, &run->expected_closed);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    if (!completed(run, xr_xir_compile_lower(run->closed, &target, &run->lowered, NULL), "owned-Closed-lower")) return false;
    xr_xir_compile_artifact_free(run->closed); run->closed = NULL;
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->lowered, NULL), "detached-Lowered-full-verify"));
    construction_owner(run, run->lowered, "detached-Lowered-full-v2");
    ReceiverRoles actual = receiver_shape(run->lowered);
    /* Specialization may renumber functions. Runtime uses current Lowered roles. */
    run->roles = actual;
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(proof.bytes && proof.identity && proof.layouts && proof.length == run->expected_closed.length);
    CHECK(!memcmp(proof.bytes, run->expected_closed.bytes, proof.length));
    uint8_t identity[32]; xr_sha256(run->expected_closed.bytes, run->expected_closed.length, identity);
    CHECK(!memcmp(identity, proof.identity, sizeof(identity)));
    free_poisoned_packet(&run->expected_closed); poison(identity, sizeof(identity));
    puts("ProgramSource-producer-Catalog-dead=1 detached-full-v2-proof-bytes=1");
    return true;
}

typedef struct ReceiverVmOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *delegates, *entries;
    uint64_t *resumes;
    uint32_t count;
} ReceiverVmOwner;
static ReceiverVmOwner *receiver_vm_owner;
static unsigned receiver_vm_releases;

static bool callback_check(bool condition, int line, const char *expression) {
    if (condition) return true;
    receiver_observer_failed = true;
    if (!receiver_failure_line) receiver_failure_line = line;
    record_failure("CALLBACK_OBSERVATION_ASSERTION", "real-VM-callback-observation", false, 0, 0, expression);
    fprintf(stderr, "callback-observation-failure line=%d condition=%s; returning through product API\n", line, expression);
    return false;
}
#define CALLBACK(c) callback_check(!!(c), __LINE__, #c)

/* The delegate receives the admitted active view, environment and state unchanged. */
static XrXirAction observed_receiver_vm_resume(XrXirCallView *view) {
    ReceiverVmOwner *owner = receiver_vm_owner;
    if (!CALLBACK(owner && view && xr_xir_call_admission(view))) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    uint32_t function = xr_xir_call_current_entry(view->activation);
    if (!CALLBACK(function < owner->count && owner->entries[function].resume == observed_receiver_vm_resume))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    const XrXirCallEntry *delegate = &owner->delegates[function];
    if (!CALLBACK(delegate->resume && view->environment == delegate->environment && owner->resumes[function] < UINT64_MAX))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    ++owner->resumes[function];
    return delegate->resume(view);
}
static void dispose_receiver_vm_owner(ReceiverVmOwner *owner) {
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->delegates);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->resumes);
    xr_compile_resources_free(owner);
}
static void receiver_vm_release(void *pointer) {
    /* A void lease callback records errors and releases only our known owner. */
    ReceiverVmOwner *owner = receiver_vm_owner;
    (void)CALLBACK(owner && pointer == owner && !receiver_vm_releases);
    if (!owner) return;
    receiver_vm_owner = NULL; ++receiver_vm_releases;
    dispose_receiver_vm_owner(owner);
}
static void seal_receiver_vm(ReceiverRun *run) {
    CHECK(run->lowered && !run->program && !receiver_vm_owner && !receiver_vm_releases);
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    CHECK(module && module->stage == XR_XIR_LOWERED && module->declarations);
    ReceiverVmOwner *owner = owned_allocate(run->context, 1, sizeof(*owner));
    run->pending_vm_owner = owner;
    owner->count = module->function_count;
    CHECK(completed(run, xr_xir_compile_vm_bind_table(run->lowered, &owner->bindings, &owner->delegates), "public-VM-bind-table"));
    owner->entries = owned_allocate(run->context, owner->count, sizeof(*owner->entries));
    owner->resumes = owned_allocate(run->context, owner->count, sizeof(*owner->resumes));
    for (uint32_t f = 0; f < owner->count; ++f) {
        owner->entries[f] = owner->delegates[f];
        CHECK(owner->entries[f].resume && owner->entries[f].release);
        owner->entries[f].resume = observed_receiver_vm_resume;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_compile_artifact_target(run->lowered),
        owner->entries, owner->count, module->declarations, {owner, receiver_vm_release}, module->types,
        xr_xir_compile_program_proof(run->lowered)};
    XrXirStatus status = xr_xir_compile_program_seal(run->context, &spec, &run->program);
    CHECK(completed(run, status, "public-VM-Program-seal"));
    CHECK(run->program);
    owner->lowered = run->lowered; run->lowered = NULL; receiver_vm_owner = owner;
    run->pending_vm_owner = NULL;
}
static void execute_receiver_i64(ReceiverRun *run, XrXirInstance *instance, uint32_t entry, int64_t expected, unsigned expected_yields) {
    receiver_operation = run->operation = "normal-Instance-call-yield-resume-owned-result";
    CHECK(call_completed(run, xr_xir_instance_start(instance, entry, NULL, 0), XR_XIR_CALL_READY, "xr-xir-instance-start"));
    size_t polls = 0; unsigned resumed = 0;
    uint64_t yield_epoch = 0, previous_wake = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed);
        CHECK(xr_xir_call_result_valid(&result.outcome));
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        if (result.outcome.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(resumed < expected_yields && result.epoch && result.epoch != UINT64_MAX && result.outcome.wake);
            if (!yield_epoch) yield_epoch = result.epoch;
            CHECK(result.epoch == yield_epoch && result.outcome.wake != previous_wake);
            XrXirWaitRequest wait = {0};
            CHECK(call_completed(run, xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake, &wait), XR_XIR_CALL_READY, "xr-xir-instance-wait-request"));
            CHECK(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
                !wait.subject && !wait.generation && !wait.ticket);
            CHECK(call_completed(run, xr_xir_instance_resume(instance, result.epoch, result.outcome.wake), XR_XIR_CALL_READY, "xr-xir-instance-resume"));
            previous_wake = result.outcome.wake;
            /* Count the consumed suspension once; never repoll a pending wake. */
            ++resumed; continue;
        }
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_RETURNED, "Instance-poll-terminal"));
        CHECK(resumed == expected_yields);
        break;
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(call_completed(run, xr_xir_instance_take_result(instance, &run->value), XR_XIR_CALL_RETURNED, "xr-xir-instance-take-result"));
    CHECK(xr_xir_value_valid(&run->value) && run->value.type == XR_XIR_I64 && !run->value.reserved && (int64_t)run->value.payload == expected);
    xr_xir_value_drop(&run->value);
    CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload);
}
static void run_receiver_instances(ReceiverRun *run) {
    seal_receiver_vm(run);
    XrXirInstanceConfig config;
    CHECK(call_completed(run, xr_xir_instance_config_init(&config, sizeof(config)), XR_XIR_CALL_READY, "xr-xir-instance-config-init"));
    config.value_limit = UINT64_C(1048576);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(call_completed(run, xr_xir_instance_new(run->program, &config, &run->instances[i]), XR_XIR_CALL_READY, "Instance-new") && run->instances[i]);
    CHECK(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    CHECK(!receiver_observer_failed && receiver_vm_owner && !receiver_vm_releases &&
        !run->catalog && !run->product && !run->program_session);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(call_completed(run, xr_xir_instance_start(run->instances[i], run->roles.private_answer, NULL, 0), XR_XIR_CALL_BAD_ARGUMENT, "private-answer-permission-reject"));
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_receiver_i64(run, run->instances[i], run->roles.entry, 0, 0);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            uint64_t before_child = receiver_vm_owner->resumes[run->roles.child];
            uint64_t before_constructor = receiver_vm_owner->resumes[run->roles.constructor];
            execute_receiver_i64(run, run->instances[i], run->roles.answer, 42, 2);
            CHECK(receiver_vm_owner->resumes[run->roles.child] > before_child);
            CHECK(receiver_vm_owner->resumes[run->roles.constructor] > before_constructor);
            printf("typed-library-receiver instance=%u repeat=%u actual-child-VM=%llu constructor-VM=%llu actual-yield-resumes=2 owned-I64=42\n",
                i, repeat, (unsigned long long)(receiver_vm_owner->resumes[run->roles.child] - before_child),
                (unsigned long long)(receiver_vm_owner->resumes[run->roles.constructor] - before_constructor));
        }
        /* READY free applies only to these completely successful normal calls. */
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "successful-normal-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_vm_owner && !receiver_vm_releases);
    }
    CHECK(!receiver_vm_owner && receiver_vm_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}

static void cleanup_created_leaf(ReceiverRun *run, unsigned leaf) {
    XrOsIoPolicy policy = xr_os_io_system_policy();
    char current[XR_PATH_MAX]; XrFsStat stat = {0};
    bool safe = run->paths_valid && run->created[leaf] &&
        xr_os_io_stat(&policy, run->root, &stat) == XR_OS_IO_OK && stat.kind == XR_FS_DIR &&
        xr_os_io_realpath(&policy, run->root, current, sizeof(current)) == XR_OS_IO_OK && same_path(current, run->root);
    if (!safe || xr_os_io_remove(&policy, run->paths[leaf]) != XR_OS_IO_OK) {
        receiver_cleanup_failed = true;
        fprintf(stderr, "cleanup-error own-leaf=%u removal-not-proved; first error preserved\n", leaf); return;
    }
    run->created[leaf] = false;
    memset(&stat, 0xa5, sizeof(stat)); uint8_t previous[sizeof(stat)]; memcpy(previous, &stat, sizeof(stat));
    uint8_t *bytes = NULL; size_t length = 0;
    bool absent = xr_os_io_stat(&policy, run->paths[leaf], &stat) == XR_OS_IO_NOT_FOUND && !memcmp(&stat, previous, sizeof(stat)) &&
        xr_os_io_read_regular_file(&policy, run->paths[leaf], 4096, &bytes, &length) == XR_OS_IO_NOT_FOUND && !bytes && !length;
    if (bytes) policy.free(policy.context, bytes);
    if (!absent) { receiver_cleanup_failed = true; fprintf(stderr, "cleanup-error own-leaf=%u strict-NOT_FOUND-unproved\n", leaf); }
}
static void release_remaining(ReceiverRun *run) {
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) {
            receiver_cleanup_failed = true;
            fprintf(stderr, "cleanup-error Instance=%u BUSY, physical cleanup unproved\n", i);
        } else run->instances[i] = NULL;
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    if (receiver_vm_owner && !run->instances[0] && !run->instances[1]) {
        receiver_cleanup_failed = true;
        ReceiverVmOwner *owner = receiver_vm_owner; receiver_vm_owner = NULL;
        dispose_receiver_vm_owner(owner);
    }
    if (run->pending_vm_owner) {
        ReceiverVmOwner *owner = run->pending_vm_owner;
        dispose_receiver_vm_owner(owner); run->pending_vm_owner = NULL;
    }
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_artifact_free(run->closed); run->closed = NULL;
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    free_poisoned_packet(&run->expected_source); free_poisoned_packet(&run->expected_closed);
    free_poisoned_packet(&run->transient_packet);
    if (run->transient_bytes) {
        poison(run->transient_bytes, run->transient_size);
        XrOsIoPolicy policy = xr_compile_io_policy(run->context->resources);
        policy.free(policy.context, run->transient_bytes); run->transient_bytes = NULL; run->transient_size = 0;
    }
    free_text(run->transient_canonical); run->transient_canonical = NULL;
    free_text(run->transient_logical); run->transient_logical = NULL;
    xr_compile_module_id_cleanup(&run->resolved);
    free_text(run->resolver_error); run->resolver_error = NULL;
    xr_compile_module_resolver_free(run->resolver); run->resolver = NULL;
    xr_xir_compile_source_product_free(run->control_product); run->control_product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->control_diagnostic);
    xr_compile_session_free(run->control_session); run->control_session = NULL;
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_compile_session_free(run->program_session); run->program_session = NULL;
    xr_xir_compile_library_catalog_free(run->catalog); run->catalog = NULL;
    library_inputs_die(run); run->binding_count = 0;
    for (unsigned leaf = 0; leaf < 2; ++leaf)
        if (run->created[leaf]) cleanup_created_leaf(run, leaf);
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    /* Static state preserves every owned pointer across the test's failure jump. */
    static ReceiverRun run;
    run.context = source_program_owner(67108864, 128000000);
    volatile bool passed = false;
    if (setjmp(receiver_failure) == 0) {
        receiver_cleanup_enabled = true;
        copy_owned_sources(&run, argv);
        passed = library_catalog_build(&run) && program_source_build(&run) && detach_program(&run);
        if (passed) run_receiver_instances(&run);
    } else passed = false;
    receiver_cleanup_enabled = false;
    if (receiver_first_failure.domain) fprintf(stderr,
        "typed-library-receiver first-domain=%s operation=%s status-observed=%u expected=%d actual=%d line=%d expression=%s\n",
        receiver_first_failure.domain, receiver_first_failure.operation, (unsigned)receiver_first_failure.has_status,
        receiver_first_failure.expected, receiver_first_failure.actual, receiver_first_failure.line,
        receiver_first_failure.expression ? receiver_first_failure.expression : "(status-return)");
    release_remaining(&run);
    source_program_owners_free();
    bool physical_zero = !source_program_compile_live && !source_program_compile_bytes &&
        !source_program_compile_allocations && !source_program_compile_capacity &&
        !runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity;
    if (!physical_zero) fprintf(stderr, "cleanup-error physical-zero-unproved compile=%zu/%zu table=%p/%zu runtime=%zu/%zu table=%p/%zu\n",
        source_program_compile_live, source_program_compile_bytes, (void *)source_program_compile_allocations,
        source_program_compile_capacity, runtime_live, runtime_bytes, (void *)runtime_owned, runtime_owned_capacity);
    if (!physical_zero || receiver_cleanup_failed || receiver_observer_failed) passed = false;
    printf("typed-library-receiver last-XIR-status=%u normal-result=%s cleanup-error=%u physical-zero=%u full-FI=NOT_RUN axes=NOT_RUN cancel=NOT_RUN stable-object-lifecycle=OPEN\n",
        (unsigned)run.status, passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero);
    return passed ? 0 : 1;
}
