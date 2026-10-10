/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_static_methods_current_native_mixed.c - Authentic native and mixed execution
 *
 * KEY CONCEPT:
 *   An authentic writer exports the complete original static methods and actual C.
 *   A packet-only process owns its facts and observes actual native and VM execution.
 */
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
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
/* This pins a public Git reference, without granting a producer or execution window. */
_Static_assert(XR_XIR_CHECKED_SCHEMA == 28u && XR_XIR_CHECKED_CONTRACT == 73u, "Formal Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 23u && XR_XIR_CALL_ABI_VERSION == 29u &&
    XR_XIR_PROGRAM_ABI_VERSION == 30u, "Formal public runtime identity");

typedef struct ReceiverRoles { uint32_t entry, answer, private_answer, value_methods[2]; } ReceiverRoles;
typedef struct ReceiverRun {
    const XrXirCompileContext *context;
    uint8_t *transient_bytes;
    size_t transient_size;
    unsigned mode;
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    XrCompilerSession *program_session;
    XrXirSourceProduct *product;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirCheckedPacket expected_source;
    XrXirCSource emitted;
    char root[XR_PATH_MAX], paths[1][XR_PATH_MAX], stdlib[XR_PATH_MAX];
    bool paths_valid, created[1];
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
#endif
    XrXirArtifact *source_checked, *closed, *lowered;
    XrXirCheckedPacket expected_closed, transient_packet;
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

static void free_poisoned_packet(XrXirCheckedPacket *packet) {
    if (packet->bytes) poison(packet->bytes, packet->length);
    xr_xir_compile_checked_packet_free(packet);
}

static bool completed(ReceiverRun *run, XrXirStatus status, const char *operation) {
    run->operation = operation; run->status = status; receiver_operation = operation;
    if (status == XR_XIR_OK) return true;
    record_failure("PRODUCT_XIR_STATUS", operation, true, XR_XIR_OK, status, NULL);
    fprintf(stderr, "static-native-mixed-packet operation=%s required=0 actual=%u\n",
        operation, (unsigned)status);
    return false;
}

#if !defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)

static bool call_completed(ReceiverRun *run, XrXirCallStatus actual, XrXirCallStatus expected, const char *operation) {
    run->operation = operation; receiver_operation = operation;
    if (actual == expected) return true;
    record_failure("PRODUCT_CALL_STATUS", operation, true, expected, actual, NULL); return false;
}

#endif
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)

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

static void copy_owned_source(ReceiverRun *run, char **argv) {
    receiver_operation = run->operation = "full-original-Static-native-mixed-Source-CREATE_NEW";
    XrOsIoPolicy policy = xr_compile_io_policy(run->context->resources);
    char cwd[XR_PATH_MAX], build[XR_PATH_MAX], expected[XR_PATH_MAX], fixture[XR_PATH_MAX];
    CHECK(xr_os_io_getcwd(&policy, cwd, sizeof(cwd)) == XR_OS_IO_OK);
    CHECK(xr_os_io_realpath(&policy, cwd, build, sizeof(build)) == XR_OS_IO_OK);
    int size = snprintf(expected, sizeof(expected), "%s/generated/current-static-native-r1/private-source", build);
    CHECK(size > 0 && (size_t)size < sizeof(expected));
    CHECK(xr_os_io_realpath(&policy, argv[1], run->root, sizeof(run->root)) == XR_OS_IO_OK);
    CHECK(same_path(expected, run->root));
    CHECK(strlen(argv[3]) < sizeof(run->stdlib)); strcpy(run->stdlib, argv[3]);
    size = snprintf(run->paths[0], sizeof(run->paths[0]), "%s/root.xr", run->root);
    CHECK(size > 0 && (size_t)size < sizeof(run->paths[0]));
    size = snprintf(fixture, sizeof(fixture), "%s/root.xr", argv[2]);
    CHECK(size > 0 && (size_t)size < sizeof(fixture));
    run->paths_valid = true; owned_root_check(run, &policy); absent_owned_leaf(run, 0);
    CHECK(xr_os_io_read_regular_file(&policy, fixture, 220, &run->transient_bytes, &run->transient_size) == XR_OS_IO_OK);
    CHECK(run->transient_bytes && run->transient_size == 220);
    exact_digest(run->transient_bytes, 220, "f7192f6c9d1a6a6403fe142383c9302520840abaa2ccdaf3e6556959796c5fcd");
    exact_digest(run->transient_bytes, 166, "77396ef78a3b53c970c1940b9417801391ade4b4ca12dd5c6ae2688eea982cf5");
    const char adapter[] = "export fn consumerAnswer() -> i64 { return answer() }\n";
    CHECK(sizeof(adapter) - 1 == 54 && !memcmp(run->transient_bytes + 166, adapter, 54));
    CHECK(xr_os_io_write_new_file_sync(&policy, run->paths[0], run->transient_bytes, run->transient_size) == XR_OS_IO_OK);
    run->created[0] = true;
    poison(run->transient_bytes, run->transient_size); policy.free(policy.context, run->transient_bytes);
    run->transient_bytes = NULL; run->transient_size = 0;
    poison(cwd, sizeof(cwd)); poison(build, sizeof(build)); poison(expected, sizeof(expected)); poison(fixture, sizeof(fixture));
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

#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)

static void packet_file_write(ReceiverRun *run, const char *path, const void *bytes, size_t length) {
    receiver_operation = run->operation = "writer-exclusive-authentic-material-file";
    CHECK(!run->packet_file && path && bytes && length);
    unsigned slot = 0;
    while (slot < 4 && strcmp(path, run->material_paths[slot])) ++slot;
    CHECK(slot < 4 && !run->material_created[slot]);
    run->packet_file = fopen(path, "wbx");
    CHECK(run->packet_file);
    run->material_created[slot] = true;
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

#include "native_mixed_support.h"

static ReceiverRoles receiver_shape(const XrXirArtifact *artifact) {
    return native_current_roles(xr_xir_compile_artifact_module(artifact));
}
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)

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

static bool program_source_build(ReceiverRun *run) {
    CHECK(run->created[0] && run->paths_valid);
    CHECK(session_completed(xr_compile_session_new(run->context->resources, &run->program_session), "Static-native-mixed-Source-session-new"));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, run->root};
    XrXirSourceProductRequest request = {{run->program_session, run->paths[0], &authority,
        run->context, run->stdlib, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    if (!completed(run, xr_xir_compile_source_product_build(&request, &run->product,
        &run->diagnostic), "complete-Static-native-mixed-SourceProduct-build")) {
        fprintf(stderr, "Static-native-mixed-stage=%u message=%.192s path=%s\n", (unsigned)run->diagnostic.stage,
            run->diagnostic.source.message, run->diagnostic.source_path ? run->diagnostic.source_path : "(none)");
        return false;
    }
    CHECK(run->product && xr_xir_compile_source_product_context(run->product)->resources == run->context->resources);
    CHECK(completed(run, xr_xir_compile_source_product_verify(run->product, 16777216, NULL), "Static-native-mixed-SourceProduct-full-verify"));
    XrXirSourceDiagnostic admission = {0};
    CHECK(completed(run, xr_xir_compile_source_product_public_admit(run->product, &admission), "Static-native-mixed-SourceProduct-public-admit"));
    XrXirSourceProductPacketView packet = {0};
    CHECK(completed(run, xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_SOURCE, &packet), "Static-native-mixed-SOURCE-authentic-packet"));
    CHECK(completed(run, xr_xir_compile_checked_read(run->context, packet.bytes, packet.length, &run->source_checked, NULL), "Static-native-mixed-SOURCE-owned-read"));
    retain_product_packet(run->context, &packet, &run->expected_source);
    CHECK(completed(run, xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet), "Static-native-mixed-CLOSED-authentic-packet"));
    retain_product_packet(run->context, &packet, &run->expected_closed);
    CHECK(completed(run, xr_xir_compile_artifact_verify(run->source_checked, NULL), "Static-native-mixed-SOURCE-full-verify"));
    construction_owner(run, run->source_checked, "Static-native-mixed-SOURCE-Construction-v2"); run->roles = receiver_shape(run->source_checked);
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(run->product);
    CHECK(facts && facts->entry < facts->function_count && facts->target.architecture == XR_XIR_ARCH_X86_64 &&
        facts->target.abi_version == XR_XIR_VALUE_ABI_VERSION);
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_compile_session_free(run->program_session); run->program_session = NULL;
    remove_created_leaf(run, 0);
    poison(&request, sizeof(request)); poison(&authority, sizeof(authority)); poison(&packet, sizeof(packet));
    poison(run->root, sizeof(run->root)); poison(run->paths, sizeof(run->paths)); poison(run->stdlib, sizeof(run->stdlib));
    run->paths_valid = false;
    CHECK(!run->product && !run->program_session && !run->created[0]);
    return true;
}

static bool detach_program(ReceiverRun *run, char **argv) {
    CHECK(!run->product && !run->program_session);
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
    puts("Static-native-mixed-SourceProduct-session-input-paths-dead=1 detached-full-v2-proof-bytes=1");
    return true;
}

#endif
#if !defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)

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
    CHECK(xr_xir_compile_artifact_module(run->closed)->stage == XR_XIR_CHECKED);
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
    puts("receiver-input-poisoned-freed=1 Closed-owner-dead=1 same-wire-rewrite=1 full-Lowered-proof=1; independent-semantic-oracle=original-First19-Second23-distinct-static-methods-I64-42");
    return true;
}

static void execute_receiver_i64(ReceiverRun *run, XrXirInstance *instance, uint32_t entry, int64_t expected) {
    receiver_operation = run->operation = "normal-Static-native-mixed-Instance-no-yield-owned-result";
    CHECK(call_completed(run, xr_xir_instance_start(instance, entry, NULL, 0), XR_XIR_CALL_READY, "Instance-start"));
    size_t polls = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&result.outcome));
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_RETURNED, "Static-native-mixed-Instance-no-yield-terminal"));
        break;
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(call_completed(run, xr_xir_instance_take_result(instance, &run->value), XR_XIR_CALL_RETURNED, "Instance-take-result"));
    CHECK(xr_xir_value_valid(&run->value) && run->value.type == XR_XIR_I64 && !run->value.reserved && (int64_t)run->value.payload == expected);
    xr_xir_value_drop(&run->value);
    CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload);
}
static void run_receiver_instances(ReceiverRun *run, const char *c_path) {
    seal_receiver_native(run, c_path);
    XrXirInstanceConfig config;
    CHECK(call_completed(run, xr_xir_instance_config_init(&config, sizeof(config)), XR_XIR_CALL_READY, "Instance-config-init"));
    config.value_limit = UINT64_C(1048576);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(call_completed(run, xr_xir_instance_new(run->program, &config, &run->instances[i]), XR_XIR_CALL_READY, "Instance-new") && run->instances[i]);
    CHECK(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    CHECK(!receiver_observer_failed && receiver_native_owner && !receiver_native_releases);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(call_completed(run, xr_xir_instance_start(run->instances[i], run->roles.private_answer, NULL, 0), XR_XIR_CALL_BAD_ARGUMENT, "private-answer-no-allocation-reject"));
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_receiver_i64(run, run->instances[i], run->roles.entry, 0);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            MethodResumeCounts before = method_resume_counts(run->roles);
            execute_receiver_i64(run, run->instances[i], run->roles.answer, 42);
            MethodResumeCounts after = method_resume_counts(run->roles);
            check_actual_method_execution(run->mode, before, after);
            printf("actual-provider instance=%u repeat=%u mode=%u fixed-I64=42 First-native=%llu First-VM=%llu Second-native=%llu Second-VM=%llu\n",
                i, repeat, run->mode, (unsigned long long)(after.native[0] - before.native[0]),
                (unsigned long long)(after.vm[0] - before.vm[0]),
                (unsigned long long)(after.native[1] - before.native[1]),
                (unsigned long long)(after.vm[1] - before.vm[1]));
        }
        /* READY free applies only to these fully successful normal calls. */
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
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)

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

static void cleanup_failed_materials(ReceiverRun *run) {
    /* Exclusive creation proves ownership of these four leaf files only. */
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!run->material_created[slot]) continue;
        if (remove(run->material_paths[slot])) {
            receiver_cleanup_failed = true;
            fprintf(stderr, "cleanup-error material-leaf=%u removal-unproved; first error preserved\n", slot);
        } else run->material_created[slot] = false;
    }
}

#endif
static void release_remaining(ReceiverRun *run) {
    if (run->packet_file) {
        if (fclose(run->packet_file)) receiver_cleanup_failed = true;
        run->packet_file = NULL;
    }
#if !defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) {
            receiver_cleanup_failed = true;
            fprintf(stderr, "cleanup-error Instance=%u BUSY physical cleanup unproved\n", i);
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
#endif
    if (run->transient_bytes) {
        poison(run->transient_bytes, run->transient_size);
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
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
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    free_poisoned_packet(&run->expected_source);
    xr_xir_compile_c_source_free(&run->emitted);
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_compile_session_free(run->program_session); run->program_session = NULL;
    if (run->created[0]) cleanup_created_leaf(run, 0);
#endif
}
int main(int argc, char **argv) {
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    if (argc != 8) return 2;
#else
    if (argc != 5) return 2;
#endif
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    /* Static state preserves every owned pointer across the failure jump. */
    static ReceiverRun run;
#if !defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    run.mode = !strcmp(argv[1], "1") ? 1u : !strcmp(argv[1], "2") ? 2u : !strcmp(argv[1], "3") ? 3u : 0u;
    if (!run.mode) return 2;
#endif
    run.context = source_program_owner(67108864, 128000000);
    volatile bool passed = false;
    if (setjmp(receiver_failure) == 0) {
        receiver_cleanup_enabled = true;
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
        copy_owned_source(&run, argv);
        passed = program_source_build(&run) && detach_program(&run, argv);
#else
        passed = admit_packet(&run, argv);
        if (passed) run_receiver_instances(&run, argv[4]);
#endif
    } else passed = false;
    receiver_cleanup_enabled = false;
    release_remaining(&run);
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    if (!passed || receiver_cleanup_failed || receiver_first_failure.domain) cleanup_failed_materials(&run);
#endif
    if (receiver_first_failure.domain) fprintf(stderr,
        "Static-native-mixed-packet first-domain=%s operation=%s status-observed=%u expected=%d actual=%d line=%d expression=%s\n",
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
    printf("Static-native-mixed-packet last-XIR-status=%u result=%s cleanup-error=%u physical-zero=%u full-FI=NOT_RUN axes=NOT_RUN cancel=NOT_RUN stable-object-lifecycle=OPEN old-representation=OPEN AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN\n",
        (unsigned)run.status, passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero);
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
    if (passed) puts("Static-native-mixed-packet writer-runtime=NOT_RUN full-original-220B=1 SourceProduct-session-inputs-dead=1");
    else puts("Static-native-mixed-packet writer-runtime=NOT_RUN material-publication=FAIL");
#endif
    return passed ? 0 : 1;
}
