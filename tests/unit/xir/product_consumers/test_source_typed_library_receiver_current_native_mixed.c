/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_typed_library_receiver_current_native_mixed.c - Authentic Library native and mixed execution
 *
 * KEY CONCEPT:
 *   The writer compiles the complete original Library and root through a real Catalog.
 *   The receiver admits the authentic Closed packet and real host-compiled C; two
 *   Instances preserve two yields and independent I64 42 under each provider route.
 */
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER) && defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
#error "Writer and cancellation receiver are mutually exclusive"
#endif
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_library_catalog.h"
#include "base/xio_policy.h"
#include "os/os_fs.h"
#endif
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
#include "base/xsha256.h"
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
/* Public Git reference 9031dbc04d5f66ae2bcb356478a0d9450dc9ad4e pins interface facts.
 * Producer adoption and execution require separately qualified materials. */
_Static_assert(XR_XIR_CHECKED_SCHEMA == 28u && XR_XIR_CHECKED_CONTRACT == 73u, "Formal Checked identity");
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
_Static_assert(XR_XIR_LIBRARY_C_INTERFACE_VERSION == 2u, "Actual owned Catalog interface");
#endif
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 23u && XR_XIR_CALL_ABI_VERSION == 29u &&
    XR_XIR_PROGRAM_ABI_VERSION == 30u, "Formal public runtime identity");

typedef struct ReceiverRoles {
    uint32_t entry, answer, private_answer, child, constructor, nominal, library_module;
} ReceiverRoles;
typedef struct ReceiverRun {
    const XrXirCompileContext *context;
    unsigned mode;
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    XrCompilerSession *library_session, *program_session;
    XrXirSourceResult library_source;
    XrXirSourceDiagnostic library_diagnostic;
    char *library_failure_path;
    XrXirCheckedPacket library_packet, expected_source;
    XrXirLibraryModuleInput *bindings;
    XrXirFunctionIdentity *control_identities;
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
    char *transient_canonical, *transient_logical;
    char root[XR_PATH_MAX], paths[2][XR_PATH_MAX], stdlib[XR_PATH_MAX];
    bool paths_valid, created[2], library_removed;
    XrXirCSource emitted;
    char material_paths[4][XR_PATH_MAX];
    bool material_created[4];
#else
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirValue value;
    struct ReceiverNativeOwner *pending_native_owner;
    XrXirCheckedPacket input_packet;
    uint8_t input_identity[32], proof_identity[32];
    char packet_path[2048], identity_path[2048];
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
    uint64_t last_call_epoch;
    unsigned completed_fixed42[2], completed_cancel_prefix[2][2];
#endif
#endif
    XrXirArtifact *source_checked, *closed, *lowered;
    XrXirCheckedPacket expected_closed, transient_packet;
    uint8_t *transient_bytes;
    size_t transient_size;
    FILE *packet_file;
    ReceiverRoles roles;
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
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static char *owned_text(const XrXirCompileContext *context, const char *text, size_t length) {
    CHECK(length < XR_PATH_MAX);
    char *copy = owned_allocate(context, length + 1, 1);
    memcpy(copy, text, length); return copy;
}
static void free_text(const char *text) {
    if (!text) return;
    poison((void *)text, strlen(text) + 1); xr_compile_resources_free((void *)text);
}
#endif

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
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static bool compile_expected(ReceiverRun *run, XrXirStatus status, XrXirStatus expected, const char *operation) {
    run->operation = operation; run->status = status; receiver_operation = operation;
    if (status == expected) return true;
    record_failure("PRODUCT_XIR_STATUS", operation, true, expected, status, NULL); return false;
}
#endif

#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static bool call_completed(ReceiverRun *run, XrXirCallStatus actual, XrXirCallStatus expected, const char *operation) {
    run->operation = operation; receiver_operation = operation;
    if (actual == expected) return true;
    record_failure("PRODUCT_CALL_STATUS", operation, true, expected, actual, NULL); return false;
}
#endif

#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
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
    int size = snprintf(expected, sizeof(expected), "%s/generated/current-typed-library-native-mixed-r1/private-source", build);
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
        if (!leaf) {
            exact_digest(run->transient_bytes, 109, "5765d2986533f38100664640b32772722b00c0f32db226ec3f8267d19c687b43");
            const char adapter[] = "export fn consumerAnswer() -> i64 { return answer() }\n";
            CHECK(sizeof(adapter) - 1 == 54 && !memcmp(run->transient_bytes + 109, adapter, 54));
        }
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
#endif

static void packet_file_close(ReceiverRun *run) {
    CHECK(run->packet_file);
    int status = fclose(run->packet_file); run->packet_file = NULL;
    CHECK(status == 0);
}

#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static void packet_file_write(ReceiverRun *run, const char *path, const void *bytes, size_t length) {
    receiver_operation = run->operation = "writer-exclusive-authentic-native-material-file";
    CHECK(!run->packet_file && path && bytes && length);
    unsigned slot = 0;
    while (slot < 4 && strcmp(path, run->material_paths[slot])) ++slot;
    CHECK(slot < 4 && !run->material_created[slot]);
    run->packet_file = fopen(path, "wbx");
    CHECK(run->packet_file); run->material_created[slot] = true;
    CHECK(fwrite(bytes, 1, length, run->packet_file) == length);
    packet_file_close(run);
}
#endif

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

#include "typed_library_native_mixed_support.h"

#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
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
/* Importing a suspension does not authorize a false no-suspend promise.
 * Only this independently owned identity table changes; the real body and
 * construction remain the original fully checked Library. */
static void receiver_suspend_promise_reject(ReceiverRun *run) {
    const XrXirArtifact *artifact = run->library_source.checked;
    XrXirModule module = *xr_xir_compile_artifact_module(artifact);
    XrXirDeclarations declarations = *module.declarations;
    ReceiverRoles roles = receiver_shape(artifact);
    CHECK(module.function_count && module.function_count <= 10000);
    run->control_identities = owned_allocate(run->context, module.function_count, sizeof(*run->control_identities));
    memcpy(run->control_identities, declarations.functions, module.function_count * sizeof(*run->control_identities));
    declarations.functions = run->control_identities; module.declarations = &declarations;
    run->control_identities[roles.child].promises |= XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_verify_v2(run->context, &module,
        xr_xir_compile_artifact_construction(artifact), &diagnostic);
    CHECK(status == XR_XIR_BAD_TYPE && diagnostic.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND && diagnostic.function == roles.child);
    xr_compile_resources_free(run->control_identities); run->control_identities = NULL;
    CHECK(completed(run, xr_xir_compile_artifact_verify(artifact, NULL), "original-Library-after-false-promise-reject"));
    puts("Library-suspension-false-NO_SUSPEND-promise-rejected=1 original-owned-body-unchanged=1");
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
    receiver_suspend_promise_reject(run);
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
    remove_created_leaf(run, 0);
    CHECK(!run->created[0] && !run->created[1]);
    poison(run->root, sizeof(run->root)); poison(run->paths, sizeof(run->paths)); poison(run->stdlib, sizeof(run->stdlib));
    run->paths_valid = false;
    return true;
}
static bool detach_program(ReceiverRun *run, char **argv) {
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
    emit_native_material(run, argv);
    free_poisoned_packet(&run->expected_closed); poison(identity, sizeof(identity));
    puts("ProgramSource-producer-Catalog-own-Source-leaves-dead=1 detached-full-v2-proof-bytes=1");
    return true;
}

#endif

#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
/* File bytes are a borrowed admission input, not an independently encoded oracle. */
static void load_packet_input(ReceiverRun *run, char **argv) {
    receiver_operation = run->operation = "receiver-packet-full-identity-input";
    CHECK(!run->packet_file && !run->input_packet.bytes && !run->input_packet.length);
    CHECK(strlen(argv[2]) < sizeof(run->packet_path) && strlen(argv[3]) < sizeof(run->identity_path));
    strcpy(run->packet_path, argv[2]); strcpy(run->identity_path, argv[3]);
    run->packet_file = fopen(run->packet_path, "rb");
    CHECK(run->packet_file && !fseek(run->packet_file, 0, SEEK_END));
    long length = ftell(run->packet_file);
    CHECK(length >= 64 && length <= 16777216 && !fseek(run->packet_file, 0, SEEK_SET));
    run->input_packet.length = (size_t)length;
    run->input_packet.bytes = owned_allocate(run->context, run->input_packet.length, 1);
    CHECK(fread(run->input_packet.bytes, 1, run->input_packet.length, run->packet_file) == run->input_packet.length);
    CHECK(fgetc(run->packet_file) == EOF && !ferror(run->packet_file));
    packet_file_close(run);
    run->packet_file = fopen(run->identity_path, "rb");
    CHECK(run->packet_file && fread(run->input_identity, 1, 32, run->packet_file) == 32);
    CHECK(fgetc(run->packet_file) == EOF && !ferror(run->packet_file));
    packet_file_close(run);
    uint8_t digest[32]; xr_sha256(run->input_packet.bytes, run->input_packet.length, digest);
    CHECK(!memcmp(digest, run->input_identity, sizeof(digest)));
    memcpy(run->proof_identity, run->input_identity, sizeof(run->proof_identity));
    poison(digest, sizeof(digest));
}
static void packet_input_die(ReceiverRun *run) {
    CHECK(run->input_packet.bytes && run->input_packet.length);
    poison(run->input_packet.bytes, run->input_packet.length);
    CHECK(run->input_packet.bytes[0] == 0xa5 && run->input_packet.bytes[run->input_packet.length - 1] == 0xa5);
    xr_xir_compile_checked_packet_free(&run->input_packet);
    poison(run->input_identity, sizeof(run->input_identity));
    poison(run->packet_path, sizeof(run->packet_path)); poison(run->identity_path, sizeof(run->identity_path));
}
static bool admit_packet(ReceiverRun *run, char **argv) {
    load_packet_input(run, argv);
    if (!completed(run, xr_xir_compile_checked_read(run->context, run->input_packet.bytes,
        run->input_packet.length, &run->closed, NULL), "receiver-public-Closed-packet-read")) return false;
    CHECK(xr_xir_compile_artifact_module(run->closed)->stage == XR_XIR_CLOSED);
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->closed, NULL), "receiver-Closed-full-verify"));
    construction_owner(run, run->closed, "receiver-Closed-real-Construction-full-v2");
    run->roles = receiver_shape(run->closed);
    CHECK(completed(run, xr_xir_compile_checked_write(run->closed, &run->expected_closed, NULL), "receiver-owned-same-wire-rewrite"));
    CHECK(run->expected_closed.bytes && run->expected_closed.bytes != run->input_packet.bytes &&
        run->expected_closed.length == run->input_packet.length &&
        !memcmp(run->expected_closed.bytes, run->input_packet.bytes, run->input_packet.length));
    packet_input_die(run);
    CHECK(!run->input_packet.bytes && !run->input_packet.length);
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->closed, NULL), "receiver-poisoned-input-dead-Closed-full-verify"));
    construction_owner(run, run->closed, "receiver-input-dead-Construction-full-v2");
    (void)receiver_shape(run->closed);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    if (!completed(run, xr_xir_compile_lower(run->closed, &target, &run->lowered, NULL), "receiver-owned-Closed-lower")) return false;
    xr_xir_compile_artifact_free(run->closed); run->closed = NULL;
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->lowered, NULL), "receiver-Closed-dead-Lowered-full-verify"));
    construction_owner(run, run->lowered, "receiver-Lowered-real-Construction-full-v2");
    run->roles = receiver_shape(run->lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(proof.bytes && proof.identity && proof.layouts && proof.length == run->expected_closed.length);
    CHECK(!memcmp(proof.bytes, run->expected_closed.bytes, proof.length));
    CHECK(!memcmp(proof.identity, run->proof_identity, sizeof(run->proof_identity)));
    free_poisoned_packet(&run->expected_closed); poison(run->proof_identity, sizeof(run->proof_identity));
    proof = (XrXirProgramProof){0};
    puts("receiver-input-poisoned-freed=1 Closed-owner-dead=1 same-wire-rewrite=1 full-Lowered-proof=1; independent-semantic-oracle=Worker40-child2-two-yields-I64-42");
    return true;
}
#endif

#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static void execute_receiver_i64(ReceiverRun *run, XrXirInstance *instance, uint32_t entry, int64_t expected, unsigned expected_yields) {
    receiver_operation = run->operation = "normal-Instance-call-yield-resume-owned-result";
    CHECK(call_completed(run, xr_xir_instance_start(instance, entry, NULL, 0), XR_XIR_CALL_READY, "xr-xir-instance-start"));
    size_t polls = 0; unsigned resumed = 0;
    uint64_t yield_epoch = 0, previous_wake = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
        if (polls == 1) {
            CHECK(result.epoch && result.epoch != UINT64_MAX); run->last_call_epoch = result.epoch;
        } else CHECK(result.epoch == run->last_call_epoch);
#endif
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
#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
static void run_receiver_instances(ReceiverRun *run, const char *c_path) {
    seal_receiver_native(run, c_path);
    XrXirInstanceConfig config;
    CHECK(call_completed(run, xr_xir_instance_config_init(&config, sizeof(config)), XR_XIR_CALL_READY, "xr-xir-instance-config-init"));
    config.value_limit = UINT64_C(1048576);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(call_completed(run, xr_xir_instance_new(run->program, &config, &run->instances[i]), XR_XIR_CALL_READY, "Instance-new") && run->instances[i]);
    CHECK(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    CHECK(!receiver_observer_failed && receiver_native_owner && !receiver_native_releases);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(call_completed(run, xr_xir_instance_start(run->instances[i], run->roles.private_answer, NULL, 0), XR_XIR_CALL_BAD_ARGUMENT, "private-answer-permission-reject"));
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_receiver_i64(run, run->instances[i], run->roles.entry, 0, 0);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            NativeRouteCounts before = native_route_counts(run->roles);
            execute_receiver_i64(run, run->instances[i], run->roles.answer, 42, 2);
            NativeRouteCounts after = native_route_counts(run->roles);
            check_actual_route(run->mode, before, after);
            printf("typed-library-native-mixed instance=%u repeat=%u mode=%u constructor-native=%llu constructor-VM=%llu child-native=%llu child-VM=%llu actual-yield-resumes=2 owned-I64=42\n",
                i, repeat, run->mode, (unsigned long long)(after.native[0] - before.native[0]),
                (unsigned long long)(after.vm[0] - before.vm[0]), (unsigned long long)(after.native[1] - before.native[1]),
                (unsigned long long)(after.vm[1] - before.vm[1]));
        }
        /* READY free applies only to these completely successful normal calls. */
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "successful-normal-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_native_owner && !receiver_native_releases);
    }
    CHECK(!receiver_native_owner && receiver_native_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}
#endif
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
static XrXirInstanceResult reach_cancel_prefix(ReceiverRun *run, XrXirInstance *instance, unsigned prefix) {
    CHECK(prefix == 1 || prefix == 2);
    CHECK(call_completed(run, xr_xir_instance_start(instance, run->roles.answer, NULL, 0),
        XR_XIR_CALL_READY, "cancel-prefix-real-answer-start"));
    size_t polls = 0; unsigned suspended = 0, resumed = 0;
    uint64_t epoch = 0, previous_wake = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&result.outcome));
        CHECK(result.epoch && result.epoch != UINT64_MAX);
        if (!epoch) epoch = result.epoch;
        CHECK(result.epoch == epoch);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_SUSPENDED, "cancel-prefix-real-suspension"));
        CHECK(result.outcome.wake && result.outcome.wake != previous_wake);
        XrXirWaitRequest wait = {0};
        CHECK(call_completed(run, xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake, &wait),
            XR_XIR_CALL_READY, "cancel-prefix-public-YIELD-request"));
        CHECK(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
            !wait.subject && !wait.generation && !wait.ticket);
        ++suspended;
        if (suspended == prefix) { CHECK(resumed == prefix - 1); return result; }
        CHECK(suspended < prefix);
        CHECK(call_completed(run, xr_xir_instance_resume(instance, result.epoch, result.outcome.wake),
            XR_XIR_CALL_READY, "cancel-prefix-resume-earlier-wake-once"));
        previous_wake = result.outcome.wake; ++resumed;
    }
}
static void reject_cancelled_token(ReceiverRun *run, XrXirInstance *instance, XrXirInstanceResult pending) {
    XrXirWaitRequest sentinel; uint8_t before[sizeof(sentinel)];
    memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(before, &sentinel, sizeof(sentinel));
    CHECK(call_completed(run, xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake, &sentinel),
        XR_XIR_CALL_BAD_STATE, "cancelled-token-public-wait-reject"));
    CHECK(!memcmp(&sentinel, before, sizeof(sentinel)));
    CHECK(call_completed(run, xr_xir_instance_resume(instance, pending.epoch, pending.outcome.wake),
        XR_XIR_CALL_BAD_STATE, "cancelled-token-public-resume-reject"));
}
static void drain_clean_cancel(ReceiverRun *run, XrXirInstance *instance, XrXirInstanceResult pending) {
    CHECK(call_completed(run, xr_xir_instance_cancel_current(instance), XR_XIR_CALL_CANCEL_REQUESTED,
        "clean-prefix-public-cancel-current"));
    reject_cancelled_token(run, instance, pending);
    size_t polls = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&result.outcome) && result.epoch == pending.epoch);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_CANCELLED, "clean-prefix-natural-terminal-CANCELLED"));
        CHECK(result.outcome.value.type == XR_XIR_UNIT && !result.outcome.value.reserved &&
            !result.outcome.value.payload && !result.outcome.wake && xr_xir_panic_empty(&result.outcome.panic));
        break;
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    reject_cancelled_token(run, instance, pending);
    CHECK(receiver_native_owner && !receiver_native_releases);
    /* Poll outcomes borrow; cancellation transfers no owned I64 and is never dropped. */
}
static void cancelled_output_diagnostics(ReceiverRun *run, XrXirInstance *instance) {
    /* Auxiliary current API controls do not redefine the original CANCELLED terminal. */
    CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload);
    CHECK(call_completed(run, xr_xir_instance_take_result(instance, &run->value), XR_XIR_CALL_BAD_STATE,
        "clean-cancel-empty-take-auxiliary"));
    CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload);
    XrXirValue sentinel = {0}; sentinel.type = XR_XIR_I64; sentinel.payload = 123;
    uint8_t before[sizeof(sentinel)]; memcpy(before, &sentinel, sizeof(sentinel));
    CHECK(call_completed(run, xr_xir_instance_take_result(instance, &sentinel), XR_XIR_CALL_BAD_ARGUMENT,
        "clean-cancel-occupied-take-auxiliary"));
    CHECK(!memcmp(&sentinel, before, sizeof(sentinel)));
}
static void run_cancel_case(ReceiverRun *run, XrXirInstance *instance, XrXirInstance *peer, unsigned index, unsigned prefix) {
    CHECK(index < 2 && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY &&
        xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY);
    NativeRouteCounts before = native_route_counts(run->roles);
    XrXirInstanceResult pending = reach_cancel_prefix(run, instance, prefix);
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY);
    drain_clean_cancel(run, instance, pending);
    NativeRouteCounts after = native_route_counts(run->roles);
    check_actual_cancel_route(run->mode, prefix, before, after);
    cancelled_output_diagnostics(run, instance);
    ++run->completed_cancel_prefix[index][prefix - 1];
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY && receiver_native_owner && !receiver_native_releases);
    before = native_route_counts(run->roles);
    execute_receiver_i64(run, instance, run->roles.answer, 42, 2);
    CHECK(run->last_call_epoch == pending.epoch + 1);
    reject_cancelled_token(run, instance, pending);
    after = native_route_counts(run->roles); check_actual_route(run->mode, before, after);
    ++run->completed_fixed42[index];
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY && receiver_native_owner && !receiver_native_releases);
    printf("typed-library-clean-cancel instance=%u mode=%u prefix=%u actual-SUSPEND=%u actual-earlier-resumes=%u terminal=CANCELLED recovered-owned-I64=42 new-epoch=1 lease=0 concurrent-pending=DEFERRED\n",
        index, run->mode, prefix, prefix, prefix - 1);
}
static void prepare_cancel_instances(ReceiverRun *run, const char *c_path) {
    seal_receiver_native(run, c_path);
    XrXirInstanceConfig config;
    CHECK(call_completed(run, xr_xir_instance_config_init(&config, sizeof(config)), XR_XIR_CALL_READY, "cancel-Instance-config-init"));
    config.value_limit = UINT64_C(1048576);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(call_completed(run, xr_xir_instance_new(run->program, &config, &run->instances[i]), XR_XIR_CALL_READY, "cancel-Instance-new") && run->instances[i]);
    CHECK(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    CHECK(!receiver_observer_failed && receiver_native_owner && !receiver_native_releases);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(call_completed(run, xr_xir_instance_start(run->instances[i], run->roles.private_answer, NULL, 0),
            XR_XIR_CALL_BAD_ARGUMENT, "cancel-private-answer-permission-reject"));
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_receiver_i64(run, run->instances[i], run->roles.entry, 0, 0);
    }
    CHECK(xr_xir_instance_state(run->instances[0]) == XR_XIR_INSTANCE_READY &&
        xr_xir_instance_state(run->instances[1]) == XR_XIR_INSTANCE_READY);
}
static void run_receiver_cancel_instances(ReceiverRun *run, const char *c_path) {
    prepare_cancel_instances(run, c_path);
    for (unsigned i = 0; i < 2; ++i) {
        NativeRouteCounts before = native_route_counts(run->roles);
        execute_receiver_i64(run, run->instances[i], run->roles.answer, 42, 2);
        NativeRouteCounts after = native_route_counts(run->roles); check_actual_route(run->mode, before, after);
        ++run->completed_fixed42[i];
        for (unsigned prefix = 1; prefix <= 2; ++prefix)
            run_cancel_case(run, run->instances[i], run->instances[1 - i], i, prefix);
        CHECK(run->completed_fixed42[i] == 3 && run->completed_cancel_prefix[i][0] == 1 &&
            run->completed_cancel_prefix[i][1] == 1 && receiver_native_owner && !receiver_native_releases);
    }
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_READY);
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "clean-prefix-successful-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_native_owner && !receiver_native_releases);
    }
    CHECK(!receiver_native_owner && receiver_native_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}
static void report_cancel_execution(const ReceiverRun *run, bool passed, bool physical_zero) {
    printf("typed-library-clean-prefix result=%s cleanup-error=%u physical-zero=%u instance0-real42=%u instance0-cancel1=%u instance0-cancel2=%u instance1-real42=%u instance1-cancel1=%u instance1-cancel2=%u expected-per-Instance=3-real42+2-CANCELLED full-FI=NOT_RUN axes=NOT_RUN stable-object-lifecycle=OPEN init-failure=OPEN owned-escape=OPEN original-AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN concurrent-pending=DEFERRED\n",
        passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero,
        run->completed_fixed42[0], run->completed_cancel_prefix[0][0], run->completed_cancel_prefix[0][1],
        run->completed_fixed42[1], run->completed_cancel_prefix[1][0], run->completed_cancel_prefix[1][1]);
}
#endif

#endif

#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
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
#endif

#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static void cleanup_failed_materials(ReceiverRun *run) {
    /* Only successful exclusive leaf creation establishes ownership. */
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!run->material_created[slot]) continue;
        if (remove(run->material_paths[slot])) {
            receiver_cleanup_failed = true;
            fprintf(stderr, "cleanup-error material-leaf=%u removal-unproved; first error preserved\n", slot);
        } else run->material_created[slot] = false;
    }
}
#endif
#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static void release_receiver_inputs(ReceiverRun *run) {
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) {
            receiver_cleanup_failed = true;
            fprintf(stderr, "cleanup-error Instance=%u BUSY, physical cleanup unproved\n", i);
        } else {
            run->instances[i] = NULL;
            if (status != XR_XIR_CALL_READY) {
                receiver_cleanup_failed = true;
                record_failure("PRODUCT_CALL_STATUS", "failure-cleanup-Instance-free", true, XR_XIR_CALL_READY, status, NULL);
            }
        }
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    if (receiver_native_owner && !run->instances[0] && !run->instances[1]) {
        receiver_cleanup_failed = true;
        ReceiverNativeOwner *owner = receiver_native_owner; receiver_native_owner = NULL;
        dispose_receiver_native_owner(owner);
    }
    if (run->pending_native_owner) {
        ReceiverNativeOwner *owner = run->pending_native_owner;
        dispose_receiver_native_owner(owner); run->pending_native_owner = NULL;
    }
    free_poisoned_packet(&run->input_packet);
    poison(run->input_identity, sizeof(run->input_identity)); poison(run->proof_identity, sizeof(run->proof_identity));
    poison(run->packet_path, sizeof(run->packet_path)); poison(run->identity_path, sizeof(run->identity_path));
}
#else
static void release_writer_inputs(ReceiverRun *run) {
    xr_compile_resources_free(run->control_identities); run->control_identities = NULL;
    free_poisoned_packet(&run->expected_source);
    xr_xir_compile_c_source_free(&run->emitted);
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
#endif
static void release_remaining(ReceiverRun *run) {
    if (run->packet_file) {
        if (fclose(run->packet_file)) receiver_cleanup_failed = true;
        run->packet_file = NULL;
    }
#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    release_receiver_inputs(run);
#endif
    if (run->transient_bytes) {
        poison(run->transient_bytes, run->transient_size);
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
        XrOsIoPolicy policy = xr_compile_io_policy(run->context->resources);
        policy.free(policy.context, run->transient_bytes);
#else
        xr_compile_resources_free(run->transient_bytes);
#endif
        run->transient_bytes = NULL; run->transient_size = 0;
    }
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_artifact_free(run->closed); run->closed = NULL;
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    free_poisoned_packet(&run->expected_closed); free_poisoned_packet(&run->transient_packet);
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    release_writer_inputs(run);
#endif
}
int main(int argc, char **argv) {
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    if (argc != 8) return 2;
#else
    if (argc != 5) return 2;
#endif
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    /* Static state preserves every owned pointer across the test's failure jump. */
    static ReceiverRun run;
#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    run.mode = !strcmp(argv[1], "1") ? 1u : !strcmp(argv[1], "2") ? 2u : !strcmp(argv[1], "3") ? 3u : 0u;
    if (!run.mode) return 2;
#endif
    run.context = source_program_owner(67108864, 128000000);
    volatile bool passed = false;
    if (setjmp(receiver_failure) == 0) {
        receiver_cleanup_enabled = true;
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
        copy_owned_sources(&run, argv);
        passed = library_catalog_build(&run) && program_source_build(&run) && detach_program(&run, argv);
#else
        passed = admit_packet(&run, argv);
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
        if (passed) run_receiver_cancel_instances(&run, argv[4]);
#else
        if (passed) run_receiver_instances(&run, argv[4]);
#endif
#endif
    } else passed = false;
    receiver_cleanup_enabled = false;
    release_remaining(&run);
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    if (!passed || receiver_cleanup_failed || receiver_first_failure.domain) cleanup_failed_materials(&run);
#endif
    if (receiver_first_failure.domain) fprintf(stderr,
        "typed-library-receiver first-domain=%s operation=%s status-observed=%u expected=%d actual=%d line=%d expression=%s\n",
        receiver_first_failure.domain, receiver_first_failure.operation, (unsigned)receiver_first_failure.has_status,
        receiver_first_failure.expected, receiver_first_failure.actual, receiver_first_failure.line,
        receiver_first_failure.expression ? receiver_first_failure.expression : "(status-return)");
    source_program_owners_free();
    bool physical_zero = !source_program_compile_live && !source_program_compile_bytes &&
        !source_program_compile_allocations && !source_program_compile_capacity &&
        !runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity;
    if (!physical_zero) fprintf(stderr, "cleanup-error physical-zero-unproved compile=%zu/%zu table=%p/%zu runtime=%zu/%zu table=%p/%zu\n",
        source_program_compile_live, source_program_compile_bytes, (void *)source_program_compile_allocations,
        source_program_compile_capacity, runtime_live, runtime_bytes, (void *)runtime_owned, runtime_owned_capacity);
    if (!physical_zero || receiver_cleanup_failed || receiver_observer_failed) passed = false;
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
    report_cancel_execution(&run, passed, physical_zero);
#else
    printf("typed-library-native-mixed-packet last-XIR-status=%u result=%s cleanup-error=%u physical-zero=%u full-FI=NOT_RUN axes=NOT_RUN cancel=NOT_RUN stable-object-lifecycle=OPEN init-failure=OPEN owned-escape=OPEN original-AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN\n",
        (unsigned)run.status, passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero);
#endif
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    if (passed) puts("typed-Library-packet writer-runtime=NOT_RUN Source-and-Catalog-producers-released=1");
    else puts("typed-Library-packet writer-runtime=NOT_RUN material-publication=FAIL");
#endif
    return passed ? 0 : 1;
}
