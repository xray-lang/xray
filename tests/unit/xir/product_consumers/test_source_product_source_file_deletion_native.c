/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_source_file_deletion_native.c - Complete native nominal typed catch
 *
 * KEY CONCEPT:
 *   Unmodified source declarations survive producer destruction as Checked
 *   and Lowered owners; isolated instances retain their sealed code lease.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "base/xsha256.h"
#include "xir/xxir_types.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked input");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution contracts");

static const char original_source[] =
    "import { Problem as ImportedProblem, fail } from \"./library\"\n"
    "fn localFail() -> i64 { throw ImportedProblem.Failed }\n"
    "fn answer() -> i64 {\n"
    " var result = 0\n"
    " try { result = fail() } catch (e: ImportedProblem) { result = 20 }\n"
    " try { result = localFail() } catch (e: ImportedProblem) { result = result + 22 }\n"
    " assert(result == 42)\n"
    " return result\n"
    "}\n"
    "@test\n"
    "fn checkImportedIdentity() { assert(answer() == 42) }\n";

static const char library_source[] =
    "export enum Problem { Failed }\n"
    "export fn fail() -> i64 { throw Problem.Failed }\n";

typedef struct EnumRoles {
    uint32_t entry, initializer, library_initializer, answer, local_fail, fail, test, library_module;
    XrXirType problem_type;
} EnumRoles;

typedef struct DeletionRun {
    XrXirCompileContext context;
    XrCompileResourceStats baseline, final;
    XrCompilerSession *session;
    XrXirSourceProduct *product;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirDiagnostic xir;
    XrXirArtifact *source_checked, *closed_checked, *lowered;
    XrXirCheckedPacket retained;
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirValue value;
    EnumRoles roles;
    unsigned canonical_count, Unit_count, permission_count;
    XrXirCSource emitted, repeated;
    char c_digest[65];
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
    char source_root[XR_PATH_MAX], owned_path[2][XR_PATH_MAX];
    bool created[2], paths_valid, deletion_proved;
    unsigned removed_count;
} DeletionRun;

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "source-file-deletion-native check line=%d operation=%s condition=%s\n", \
        __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(DeletionRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("source-file-deletion-native phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
        " live=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 " physical=%zu/%zu\n",
        name, instance_compile_attempts, stats.allocation_count, stats.allocated_bytes,
        stats.live_bytes, stats.peak_bytes, stats.work, instance_compile_live, instance_compile_bytes);
    return true;
}

static bool named(const XrXirFunction *function, const char *name) {
    size_t size = strlen(name);
    return function->name_length == size && !memcmp(function->name, name, size);
}

static bool integer_return(const XrXirFunction *function, int64_t expected) {
    if (function->parameter_count || function->result != XR_XIR_I64) return false;
    uint32_t returns = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op != XR_XIR_RETURN) continue;
        if (op->args[0] >= function->instruction_count) return false;
        const XrXirInstruction *value = &function->instructions[op->args[0]];
        if (value->op != XR_XIR_CONST_INT || value->type != XR_XIR_I64 || value->immediate != expected)
            return false;
        ++returns;
    }
    return returns == 1;
}

static bool literal_name(XrXirLiteral value, const char *name) {
    size_t length = strlen(name);
    return value.length == length && !memcmp(value.bytes, name, length);
}

static bool inspect_enum(DeletionRun *run, const XrXirModule *module, EnumRoles *roles) {
    const XrXirTypes *types = module->types;
    const XrXirNominalTable *table = types ? types->nominals : NULL;
    OBSERVE(table && (table->declarations || table->identities));
    uint32_t declaration = UINT32_MAX;
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirLiteral name = table->declarations ? table->declarations[i].name : table->identities[i].name;
        OBSERVE(!literal_name(name, "ImportedProblem"));
        if (!literal_name(name, "Problem")) continue;
        OBSERVE(declaration == UINT32_MAX); declaration = i;
        uint32_t kind = table->declarations ? table->declarations[i].kind : table->identities[i].kind;
        uint32_t arity = table->declarations ? table->declarations[i].parameter_count : table->identities[i].arity;
        uint32_t fields = table->declarations ? table->declarations[i].field_count : table->identities[i].field_count;
        uint32_t count = table->declarations ? table->declarations[i].variant_count : table->identities[i].variant_count;
        uint32_t exported = table->declarations ? table->declarations[i].exported : table->identities[i].exported;
        XrXirLiteral owner = table->declarations ? table->declarations[i].module : table->identities[i].module;
        const XrXirNominalVariant *variants = table->declarations ? table->declarations[i].variants : table->identities[i].variants;
        const XrXirSourceModule *library = &module->declarations->modules[roles->library_module];
        OBSERVE(kind == XR_XIR_NOMINAL_ENUM && !arity && !fields && count == 1 && exported == 1);
        OBSERVE(owner.length == library->name_length && !memcmp(owner.bytes, library->name, owner.length));
        OBSERVE(variants && literal_name(variants[0].name, "Failed") && !variants[0].field_count);
    }
    OBSERVE(declaration != UINT32_MAX);
    roles->problem_type = XR_XIR_UNIT;
    for (uint32_t t = 0; t < types->count; ++t) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t);
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node || node->kind != XR_XIR_TYPE_NOMINAL || node->nominal.declaration != declaration) continue;
        OBSERVE(roles->problem_type == XR_XIR_UNIT && !node->nominal.argument_count && !node->nominal.field_count);
        OBSERVE(xr_xir_type_is_enum(types, type)); roles->problem_type = type;
    }
    OBSERVE(roles->problem_type != XR_XIR_UNIT);
    return true;
}

static bool inspect_error_identity(DeletionRun *run, const XrXirModule *module, const EnumRoles *roles) {
    if (module->stage != XR_XIR_CHECKED) return true;
    uint32_t builders[2] = {roles->local_fail, roles->fail};
    for (unsigned f = 0; f < 2; ++f) {
        const XrXirFunction *fn = &module->functions[builders[f]];
        unsigned created = 0;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op != XR_XIR_ENUM_NEW) continue;
            OBSERVE(op->type == roles->problem_type && !op->immediate); ++created;
        }
        OBSERVE(created == 1);
    }
    const XrXirFunction *answer = &module->functions[roles->answer];
    unsigned tested = 0, narrowed = 0;
    for (uint32_t i = 0; i < answer->instruction_count; ++i) {
        const XrXirInstruction *op = &answer->instructions[i];
        if (op->op == XR_XIR_ERROR_IS) { OBSERVE(op->immediate == roles->problem_type); ++tested; }
        if (op->op == XR_XIR_ERROR_NARROW) { OBSERVE(op->type == roles->problem_type); ++narrowed; }
    }
    OBSERVE(tested == 2 && narrowed == 2);
    return true;
}

static bool inspect_module(DeletionRun *run, const XrXirArtifact *artifact,
                           XrXirStage stage, EnumRoles *roles) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    OBSERVE(module && module->stage == stage && d && d->root_module < d->module_count);
    *roles = (EnumRoles){d->entry_function, d->modules[d->root_module].initializer,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_UNIT};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module == d->root_module && (named(fn, "answer") || named(fn, "localFail"))) {
            uint32_t *slot = named(fn, "answer") ? &roles->answer : &roles->local_fail;
            OBSERVE(*slot == UINT32_MAX && !identity->exported && !identity->test_role);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_I64); *slot = f;
        } else if (identity->module == d->root_module && named(fn, "checkImportedIdentity")) {
            OBSERVE(roles->test == UINT32_MAX && !identity->exported);
            OBSERVE(identity->test_role == XR_XIR_TEST_ROLE_TEST && !identity->test_timeout_seconds);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_UNIT); roles->test = f;
        } else if (named(fn, "fail")) {
            OBSERVE(identity->module != d->root_module && identity->module < d->module_count);
            OBSERVE(roles->fail == UINT32_MAX && identity->exported == 1 && !identity->test_role);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_I64);
            roles->fail = f; roles->library_module = identity->module;
        }
    }
    OBSERVE(roles->answer != UINT32_MAX && roles->local_fail != UINT32_MAX && roles->test != UINT32_MAX);
    OBSERVE(roles->fail != UINT32_MAX && roles->library_module < d->module_count);
    roles->library_initializer = d->modules[roles->library_module].initializer;
    uint32_t distinct[7] = {roles->entry, roles->initializer, roles->library_initializer,
        roles->answer, roles->local_fail, roles->fail, roles->test};
    for (unsigned i = 0; i < 7; ++i) {
        OBSERVE(distinct[i] < module->function_count);
        for (unsigned j = i + 1; j < 7; ++j) OBSERVE(distinct[i] != distinct[j]);
    }
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    uint32_t initializers[2] = {roles->initializer, roles->library_initializer};
    for (unsigned i = 0; i < 2; ++i) {
        OBSERVE(module->functions[initializers[i]].result == XR_XIR_UNIT);
        OBSERVE(!d->functions[initializers[i]].exported && !d->functions[initializers[i]].test_role);
    }
    OBSERVE(xr_xir_module_imports(d, d->root_module, roles->library_module));
    OBSERVE(!xr_xir_module_imports(d, roles->library_module, d->root_module));
    if (!inspect_enum(run, module, roles) || !inspect_error_identity(run, module, roles)) return false;
    printf("source-file-deletion-native metadata stage=%u root=%u library=%u entry=%u initializer=%u library-initializer=%u "
        "answer=%u local-fail=%u exported-fail=%u test=%u Problem-type=%u variant=Failed payload-fields=0 "
        "alias=one-declaration typed-catch-pairs=%u catch-opcode-check=%s expected-entry=0 expected-answer=42 expected-test=Unit\n",
        (unsigned)stage, d->root_module, roles->library_module, roles->entry, roles->initializer,
        roles->library_initializer, roles->answer, roles->local_fail, roles->fail, roles->test, (unsigned)roles->problem_type,
        stage == XR_XIR_CHECKED ? 2u : 0u, stage == XR_XIR_CHECKED ? "CHECKED_TWO_PAIRS" : "LOWERED_IDENTITY_ONLY");
    return true;
}

static bool inspect_product(DeletionRun *run, const char *file, const char *library) {
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(run->product);
    const XrXirSourceView *view = xr_xir_compile_source_product_view(run->product);
    const XrXirSourceTests *tests = xr_xir_compile_source_product_tests(run->product);
    const XrXirModule *module = xr_xir_compile_artifact_module(run->closed_checked);
    OBSERVE(facts && view && view->complete && tests && module);
    const XrXirCompileContext *context = xr_xir_compile_source_product_context(run->product);
    OBSERVE(context && context->resources == run->context.resources);
    OBSERVE(facts->target.architecture == XR_XIR_ARCH_X86_64 && facts->target.abi_version == XR_XIR_VALUE_ABI_VERSION);
    OBSERVE(facts->entry == run->roles.entry && facts->function_count == module->function_count);
    OBSERVE(facts->module_count == module->declarations->module_count);
    OBSERVE(tests->count == 1 && tests->entries && tests->entries[0].function == run->roles.test);
    OBSERVE(tests->entries[0].role == XR_XIR_TEST_ROLE_TEST && !tests->entries[0].timeout_seconds);
    OBSERVE(tests->entries[0].name_length == 21 && !memcmp(tests->entries[0].name, "checkImportedIdentity", 21));
    uint32_t root_matches = 0, library_matches = 0;
    for (uint32_t m = 0; m < view->module_count; ++m) {
        const char *path = view->modules[m].path;
        if (!path) continue;
        const char *expected[2] = {file, library};
        for (unsigned k = 0; k < 2; ++k) {
            if (strlen(path) != strlen(expected[k])) continue;
            bool same = true;
            for (size_t i = 0; path[i]; ++i)
                if ((path[i] == '\\' ? '/' : path[i]) != (expected[k][i] == '\\' ? '/' : expected[k][i])) same = false;
            if (same) { if (!k) ++root_matches; else ++library_matches; }
        }
    }
    OBSERVE(root_matches == 1 && library_matches == 1);
    return true;
}

static bool exact_input(DeletionRun *run, const char *file, const char *expected, size_t length) {
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    XrOsIoStatus io = xr_os_io_read_regular_file(&policy, file, length, &run->input, &run->input_length);
    OBSERVE(io == XR_OS_IO_OK && run->input_length == length && !memcmp(run->input, expected, length));
    xr_compile_resources_free(run->input); run->input = NULL;
    return true;
}

static bool same_path(const char *left, const char *right) {
    for (; *left && *right; ++left, ++right) {
        unsigned char a = (unsigned char)*left, b = (unsigned char)*right;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#if defined(XR_OS_WINDOWS)
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
#endif
        if (a != b) return false;
    }
    return !*left && !*right;
}

static bool check_owned_root(DeletionRun *run, const XrOsIoPolicy *policy) {
    char current[XR_PATH_MAX];
    XrFsStat stat = {0};
    OBSERVE(xr_os_io_stat(policy, run->source_root, &stat) == XR_OS_IO_OK);
    OBSERVE(stat.kind == XR_FS_DIR);
    OBSERVE(xr_os_io_realpath(policy, run->source_root, current, sizeof(current)) == XR_OS_IO_OK);
    OBSERVE(same_path(current, run->source_root));
    return true;
}

static bool save_owned_paths(DeletionRun *run, const char *root, const char *main, const char *library) {
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    char cwd[XR_PATH_MAX], build[XR_PATH_MAX], expected[XR_PATH_MAX], canonical[XR_PATH_MAX];
    OBSERVE(xr_os_io_getcwd(&policy, cwd, sizeof(cwd)) == XR_OS_IO_OK);
    OBSERVE(xr_os_io_realpath(&policy, cwd, build, sizeof(build)) == XR_OS_IO_OK);
    int count = snprintf(expected, sizeof(expected), "%s/generated/source-file-deletion-native1/private-source", build);
    OBSERVE(count >= 0 && (size_t)count < sizeof(expected));
    OBSERVE(xr_os_io_realpath(&policy, root, canonical, sizeof(canonical)) == XR_OS_IO_OK);
    OBSERVE(same_path(expected, canonical));
    size_t length = strlen(canonical);
    OBSERVE(length < sizeof(run->source_root));
    memcpy(run->source_root, canonical, length + 1u);
    const char *leaves[2] = {"main.xr", "library.xr"};
    const char *arguments[2] = {main, library};
    for (unsigned i = 0; i < 2; ++i) {
        count = snprintf(run->owned_path[i], sizeof(run->owned_path[i]), "%s/%s", run->source_root, leaves[i]);
        OBSERVE(count >= 0 && (size_t)count < sizeof(run->owned_path[i]));
        OBSERVE(same_path(run->owned_path[i], arguments[i]));
    }
    run->paths_valid = true;
    return check_owned_root(run, &policy);
}

static bool absent_leaf(DeletionRun *run, const XrOsIoPolicy *policy, unsigned leaf, const char *checkpoint) {
    XrFsStat stat;
    memset(&stat, 0xA5, sizeof(stat));
    stat.kind = XR_FS_OTHER; stat.size = UINT64_MAX; stat.mtime_ns = INT64_MAX;
    uint8_t snapshot[sizeof(stat)];
    memcpy(snapshot, &stat, sizeof(stat));
    XrOsIoStatus stat_status = xr_os_io_stat(policy, run->owned_path[leaf], &stat);
    bool stat_unchanged = !memcmp(snapshot, &stat, sizeof(stat));
    uint8_t *bytes = NULL;
    size_t length = 0;
    XrOsIoStatus read_status = xr_os_io_read_regular_file(policy, run->owned_path[leaf], 4096u, &bytes, &length);
    bool read_unchanged = !bytes && !length;
    if (bytes) policy->free(policy->context, bytes);
    OBSERVE(stat_status == XR_OS_IO_NOT_FOUND && stat_unchanged);
    OBSERVE(read_status == XR_OS_IO_NOT_FOUND && read_unchanged);
    if (!check_owned_root(run, policy)) return false;
    printf("source-file-deletion-native source-absent leaf=%u checkpoint=%s stat=%u read=%u stat-output=unchanged read-output=unchanged parent=DIR\n",
        leaf, checkpoint, (unsigned)stat_status, (unsigned)read_status);
    return true;
}

static bool create_owned_sources(DeletionRun *run, const char *root, const char *main, const char *library) {
    run->operation = "own-source-paths";
    if (!save_owned_paths(run, root, main, library)) return false;
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    const char *texts[2] = {original_source, library_source};
    const size_t lengths[2] = {sizeof(original_source) - 1u, sizeof(library_source) - 1u};
    for (unsigned i = 0; i < 2; ++i) {
        if (!absent_leaf(run, &policy, i, "before-CREATE_NEW")) return false;
        XrOsIoStatus status = xr_os_io_write_new_file_sync(&policy, run->owned_path[i], (const uint8_t *)texts[i], lengths[i]);
        if (status != XR_OS_IO_OK) {
            fprintf(stderr, "source-file-deletion-native CREATE_NEW failed leaf=%u status=%u path=%s created=0 unknown-residue=PRESERVE\n",
                i, (unsigned)status, run->owned_path[i]);
            return false;
        }
        run->created[i] = true;
        printf("source-file-deletion-native source-created leaf=%u bytes=%zu status=%u ownership=THIS_PROCESS_CREATE_NEW\n",
            i, lengths[i], (unsigned)status);
    }
    return true;
}

static bool remove_owned_sources(DeletionRun *run) {
    run->operation = "physical-source-delete-before-detached-Checked";
    OBSERVE(run->paths_valid && run->created[0] && run->created[1]);
    OBSERVE(!run->session && !run->product && !run->input && !run->diagnostic.snapshot && !run->diagnostic.source_path);
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    if (!check_owned_root(run, &policy)) return false;
    for (unsigned i = 0; i < 2; ++i) {
        XrOsIoStatus status = xr_os_io_remove(&policy, run->owned_path[i]);
        if (status != XR_OS_IO_OK)
            fprintf(stderr, "source-file-deletion-native remove failed leaf=%u status=%u path=%s\n", i, (unsigned)status, run->owned_path[i]);
        OBSERVE(status == XR_OS_IO_OK);
        run->created[i] = false;
        ++run->removed_count;
        printf("source-file-deletion-native source-deleted leaf=%u status=%u owner=THIS_PROCESS only-saved-leaf=1\n", i, (unsigned)status);
    }
    for (unsigned i = 0; i < 2; ++i)
        if (!absent_leaf(run, &policy, i, "before-detached-Checked")) return false;
    run->deletion_proved = true;
    return true;
}

static bool cleanup_owned_sources(DeletionRun *run) {
    bool complete = true;
    XrOsIoPolicy policy = xr_os_io_system_policy();
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->created[i]) continue;
        XrOsIoStatus status = xr_os_io_remove(&policy, run->owned_path[i]);
        if (status == XR_OS_IO_OK) run->created[i] = false;
        else {
            fprintf(stderr, "source-file-deletion-native cleanup failed leaf=%u status=%u path=%s\n", i, (unsigned)status, run->owned_path[i]);
            complete = false;
        }
    }
    return complete;
}

static bool produce(DeletionRun *run, const char *root, const char *file, const char *library) {
    run->operation = "original-two-inputs";
    if (!exact_input(run, file, original_source, sizeof(original_source) - 1) ||
        !exact_input(run, library, library_source, sizeof(library_source) - 1)) return false;
    if (!phase(run, "original-input-verified")) return false;
    run->operation = "session";
    OBSERVE(xr_compile_session_new(run->context.resources, &run->session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{run->session, file, &authority, &run->context,
        NULL, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    run->operation = "source-product";
    run->status = xr_xir_compile_source_product_build(&request, &run->product, &run->diagnostic);
    OBSERVE(run->status == XR_XIR_OK && run->product);
    return phase(run, "source-product-complete");
}

static bool read_packets(DeletionRun *run, const char *file, const char *library) {
    XrXirSourceProductPacketView source = {0}, closed = {0};
    EnumRoles source_roles;
    run->operation = "source-packet";
    run->status = xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_SOURCE, &source);
    OBSERVE(run->status == XR_XIR_OK && source.bytes && source.length);
    run->status = xr_xir_compile_checked_read(&run->context, source.bytes, source.length,
        &run->source_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    if (!inspect_module(run, run->source_checked, XR_XIR_CHECKED, &source_roles)) return false;
    if (!phase(run, "source-Checked-reader")) return false;
    run->operation = "closed-packet";
    run->status = xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &closed);
    OBSERVE(run->status == XR_XIR_OK && closed.bytes && closed.length);
    run->status = xr_xir_compile_checked_read(&run->context, closed.bytes, closed.length,
        &run->closed_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    if (!inspect_module(run, run->closed_checked, XR_XIR_CHECKED, &run->roles)) return false;
    if (!inspect_product(run, file, library)) return false;
    run->status = xr_xir_compile_checked_write(run->closed_checked, &run->retained, &run->xir);
    OBSERVE(run->status == XR_XIR_OK && run->retained.length == closed.length);
    OBSERVE(run->retained.bytes != closed.bytes && !memcmp(run->retained.bytes, closed.bytes, closed.length));
    return phase(run, "closed-Checked-retained");
}

static bool detach_lower(DeletionRun *run) {
    xr_compile_session_free(run->session); run->session = NULL;
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    if (!phase(run, "producers-destroyed")) return false;
    if (!remove_owned_sources(run)) return false;
    run->status = xr_xir_compile_artifact_verify(run->source_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    run->status = xr_xir_compile_artifact_verify(run->closed_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    XrXirArtifact *reread = NULL;
    run->status = xr_xir_compile_checked_read(&run->context, run->retained.bytes,
        run->retained.length, &reread, &run->xir);
    if (run->status != XR_XIR_OK) { xr_xir_compile_artifact_free(reread); return false; }
    run->status = xr_xir_compile_artifact_verify(reread, &run->xir);
    EnumRoles reread_roles = {0};
    bool same = run->status == XR_XIR_OK && inspect_module(run, reread, XR_XIR_CHECKED, &reread_roles);
    if (same) same = !memcmp(&run->roles, &reread_roles, sizeof(reread_roles));
    xr_xir_compile_artifact_free(reread);
    OBSERVE(same && run->status == XR_XIR_OK);
    run->operation = "Lowered";
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    EnumRoles lowered_roles;
    if (!inspect_module(run, run->lowered, XR_XIR_LOWERED, &lowered_roles)) return false;
    OBSERVE(run->roles.entry == lowered_roles.entry && run->roles.initializer == lowered_roles.initializer);
    OBSERVE(!memcmp(&run->roles, &lowered_roles, sizeof(lowered_roles)));
    run->status = xr_xir_compile_artifact_verify(run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}

#if defined(XR_SOURCE_FILE_DELETION_NATIVE)
extern const XrXirProgramSpec source_file_deletion_native_program;
extern const char source_file_deletion_native_c_sha[65];
static const XrXirProgramSpec *native_spec(void) { return &source_file_deletion_native_program; }
static const char *native_sha(void) { return source_file_deletion_native_c_sha; }
#else
static const XrXirProgramSpec *native_spec(void) { return NULL; }
static const char *native_sha(void) { return NULL; }
#endif

static void hexadecimal(const uint8_t bytes[32], char output[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 15u];
    }
    output[64] = '\0';
}

static bool emit_owned(DeletionRun *run) {
    run->operation = "emit-first";
    run->status = xr_xir_compile_emit_c(run->lowered, "source_file_deletion_native", 16777216u, &run->emitted);
    OBSERVE(run->status == XR_XIR_OK && run->emitted.text && run->emitted.length);
    if (!phase(run, "emission-first")) return false;
    run->operation = "emit-repeat";
    run->status = xr_xir_compile_emit_c(run->lowered, "source_file_deletion_native", 16777216u, &run->repeated);
    OBSERVE(run->status == XR_XIR_OK && run->repeated.text && run->repeated.length);
    OBSERVE(run->emitted.text != run->repeated.text && run->emitted.length == run->repeated.length);
    OBSERVE(!memcmp(run->emitted.text, run->repeated.text, run->emitted.length));
    uint8_t digest[32];
    xr_sha256((const uint8_t *)run->emitted.text, run->emitted.length, digest);
    hexadecimal(digest, run->c_digest);
    printf("source-file-deletion-native owned-C count=2 bytes=%zu identical=1 sha256=%s\n",
        run->emitted.length, run->c_digest);
    xr_xir_compile_c_source_free(&run->repeated);
    return phase(run, "emission-repeat-freed");
}

static bool write_translation_unit(DeletionRun *run, const char *output) {
    run->operation = "write-complete-native-TU";
    FILE *file = fopen(output, "wb");
    OBSERVE(file != NULL);
    bool written = fwrite(run->emitted.text, 1, run->emitted.length, file) == run->emitted.length;
    if (written) written = fprintf(file, "\nconst char source_file_deletion_native_c_sha[65]=\"%s\";\n", run->c_digest) > 0;
    int closed = fclose(file);
    OBSERVE(written && closed == 0);
    printf("source-file-deletion-native emitted-TU payload-sha=%s output=%s callback-ProgramSpec=1 extern-exports=OPEN\n",
        run->c_digest, output);
    return true;
}

static bool same_layout(const XrXirFunctionLayout *actual, const XrXirFunctionLayout *expected,
                        uint32_t parameters) {
    if (actual->slot_count != expected->slot_count || actual->frame_bytes != expected->frame_bytes ||
        actual->owned_count != expected->owned_count || actual->outgoing_count != expected->outgoing_count ||
        actual->path_count != expected->path_count || actual->result.size != expected->result.size ||
        actual->result.alignment != expected->result.alignment) return false;
    if (expected->slot_count && (!actual->offsets || !expected->offsets ||
        memcmp(actual->offsets, expected->offsets, (size_t)expected->slot_count * sizeof(uint32_t)))) return false;
    if (expected->owned_count && (!actual->owned_offsets || !expected->owned_offsets ||
        memcmp(actual->owned_offsets, expected->owned_offsets, (size_t)expected->owned_count * sizeof(uint32_t)))) return false;
    if (parameters && (!actual->parameters || !expected->parameters ||
        memcmp(actual->parameters, expected->parameters, (size_t)parameters * sizeof(XrXirLayout)))) return false;
    return true;
}

static bool generated_correspondence(DeletionRun *run, const XrXirProgramSpec *spec) {
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    OBSERVE(spec && module && module->declarations && proof.bytes && proof.identity && proof.layouts);
    OBSERVE(native_sha() && !strcmp(native_sha(), run->c_digest));
    OBSERVE(spec->abi_version == XR_XIR_PROGRAM_ABI_VERSION && spec->target.architecture == XR_XIR_ARCH_X86_64);
    OBSERVE(spec->target.abi_version == XR_XIR_VALUE_ABI_VERSION && spec->entry_count == module->function_count);
    OBSERVE(spec->entries && spec->declarations && !spec->code.owner && !spec->code.release);
    OBSERVE(spec->types && module->types && spec->types->count == module->types->count);
    OBSERVE(spec->proof.bytes && spec->proof.identity && spec->proof.layouts && proof.length == spec->proof.length);
    OBSERVE(!memcmp(proof.bytes, spec->proof.bytes, proof.length) && !memcmp(proof.identity, spec->proof.identity, 32));
    OBSERVE(spec->declarations->root_module == module->declarations->root_module);
    OBSERVE(spec->declarations->module_count == module->declarations->module_count);
    OBSERVE(spec->declarations->entry_function == run->roles.entry && spec->declarations->functions);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        OBSERVE(!memcmp(&spec->declarations->functions[f], &module->declarations->functions[f], sizeof(XrXirFunctionIdentity)));
        OBSERVE(same_layout(&spec->proof.layouts[f], &proof.layouts[f], module->functions[f].parameter_count));
        OBSERVE(spec->entries[f].abi_version == XR_XIR_CALL_ABI_VERSION && spec->entries[f].resume && spec->entries[f].release);
        OBSERVE(spec->entries[f].parameter_count == module->functions[f].parameter_count && spec->entries[f].result == module->functions[f].result);
        uint32_t parameters = module->functions[f].parameter_count;
        if (parameters) OBSERVE(spec->entries[f].parameters && module->functions[f].parameters &&
            !memcmp(spec->entries[f].parameters, module->functions[f].parameters, (size_t)parameters * sizeof(XrXirType)));
    }
    printf("source-file-deletion-native compiled-correspondence functions=%u all-native=1 proof-bytes=%zu layouts-exact=1 permissions-exact=1\n",
        spec->entry_count, proof.length);
    return true;
}

static bool seal_native_instances(DeletionRun *run) {
    run->operation = "native-Program-seal";
    const XrXirProgramSpec *spec = native_spec();
    if (!generated_correspondence(run, spec)) return false;
    run->status = xr_xir_compile_program_seal(&run->context, spec, &run->program);
    OBSERVE(run->status == XR_XIR_OK && run->program);
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_c_source_free(&run->emitted);
    if (!phase(run, "native-Program-sealed-C-Lowered-dropped")) return false;
    XrXirInstanceConfig config = {0};
    run->call_status = xr_xir_instance_config_init(&config, sizeof(config));
    OBSERVE(run->call_status == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        run->call_status = xr_xir_instance_new(run->program, &config, &run->instances[i]);
        OBSERVE(run->call_status == XR_XIR_CALL_READY && run->instances[i]);
    }
    OBSERVE(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    return phase(run, "native-Program-caller-dropped-two-instances-retain");
}



static bool finish(DeletionRun *run, XrXirInstance *instance, XrXirType type, int64_t payload) {
    XrXirInstanceResult result = {0};
    size_t polls = 0;
    do {
        OBSERVE(++polls <= 4096);
        result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    } while (result.outcome.status == XR_XIR_CALL_READY);
    run->call_status = result.outcome.status;
    OBSERVE(run->call_status == XR_XIR_CALL_RETURNED);
    run->call_status = xr_xir_instance_take_result(instance, &run->value);
    OBSERVE(run->call_status == XR_XIR_CALL_RETURNED);
    bool same = run->value.type == (uint32_t)type && !run->value.reserved && (int64_t)run->value.payload == payload;
    xr_xir_value_drop(&run->value);
    OBSERVE(same && !run->value.type && !run->value.reserved && !run->value.payload);
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    if (type == XR_XIR_UNIT) ++run->Unit_count; else ++run->canonical_count;
    return true;
}

static bool permission_rejected(DeletionRun *run, XrXirInstance *instance, uint32_t function, bool test) {
    XrXirCallStatus status = test ? xr_xir_instance_start_test(instance, function) :
        xr_xir_instance_start(instance, function, NULL, 0);
    OBSERVE(status == XR_XIR_CALL_BAD_ARGUMENT);
    ++run->permission_count;
    return true;
}

static bool execute_instances(DeletionRun *run) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = run->instances[i];
        run->operation = "private-entry-authority";
        size_t attempts = runtime_attempts, compiler_attempts = instance_compile_attempts;
        unsigned permissions_before = run->permission_count;
        size_t blocks = runtime_live, bytes = runtime_bytes;
        size_t compiler_live = instance_compile_live, compiler_bytes = instance_compile_bytes;
        void *table = runtime_owned;
        size_t capacity = runtime_owned_capacity;
        XrCompileResourceStats before = {0}, after = {0};
        OBSERVE(xr_compile_resources_stats(run->context.resources, &before) == XR_COMPILE_RESOURCE_OK);
        if (!permission_rejected(run, instance, run->roles.initializer, false)) return false;
        if (!permission_rejected(run, instance, run->roles.answer, false)) return false;
        if (!permission_rejected(run, instance, run->roles.local_fail, false)) return false;
        if (!permission_rejected(run, instance, run->roles.test, false)) return false;
        if (!permission_rejected(run, instance, run->roles.answer, true)) return false;
        if (!permission_rejected(run, instance, run->roles.local_fail, true)) return false;
        if (!permission_rejected(run, instance, run->roles.initializer, true)) return false;
        if (!permission_rejected(run, instance, run->roles.library_initializer, false)) return false;
        if (!permission_rejected(run, instance, run->roles.library_initializer, true)) return false;
        if (!permission_rejected(run, instance, run->roles.fail, true)) return false;
        OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
        OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
        OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
        OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
        OBSERVE(instance_compile_live == compiler_live && instance_compile_bytes == compiler_bytes);
        OBSERVE(runtime_live == blocks && runtime_bytes == bytes && runtime_owned == table &&
            runtime_owned_capacity == capacity && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
        OBSERVE(run->permission_count - permissions_before == 10);
        printf("source-file-deletion-native instance=%u permissions=%u state=NEW ledger=unchanged physical=unchanged exported-fail-ordinary=ALLOWED_NOT_EXECUTED\n", i, run->permission_count - permissions_before);
        run->operation = "canonical-entry";
        run->call_status = xr_xir_instance_start(instance, run->roles.entry, NULL, 0);
        OBSERVE(run->call_status == XR_XIR_CALL_READY);
        if (!finish(run, instance, XR_XIR_I64, 0)) return false;
        printf("source-file-deletion-native instance=%u canonical=I64 payload=0\n", i);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->operation = "same-root-checkImportedIdentity";
            run->call_status = xr_xir_instance_start_test(instance, run->roles.test);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish(run, instance, XR_XIR_UNIT, 0)) return false;
            printf("source-file-deletion-native instance=%u repeat=%u same-root-checkImportedIdentity=Unit answer-fixed-oracle=42 caught=20+22\n",
                i, repeat);
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    OBSERVE(run->canonical_count == 2 && run->Unit_count == 4 && run->permission_count == 20);
    return true;
}

static bool release(DeletionRun *run) {
    bool complete = cleanup_owned_sources(run);
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) complete = false;
        else { run->instances[i] = NULL; if (status != XR_XIR_CALL_READY) complete = false; }
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    xr_xir_compile_c_source_free(&run->emitted);
    xr_xir_compile_c_source_free(&run->repeated);
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_compile_session_free(run->session); run->session = NULL;
    xr_compile_resources_free(run->input); run->input = NULL;
    if (run->context.resources) {
        if (xr_compile_resources_stats(run->context.resources, &run->final) != XR_COMPILE_RESOURCE_OK)
            complete = false;
        if (run->final.live_bytes != run->baseline.live_bytes || run->final.live_bytes != instance_compile_bytes)
            complete = false;
        xr_compile_resources_release(run->context.resources); run->context.resources = NULL;
    }
    if (run->deletion_proved) {
        XrOsIoPolicy policy = xr_os_io_system_policy();
        for (unsigned i = 0; i < 2; ++i)
            if (!absent_leaf(run, &policy, i, "after-all-owners-released")) complete = false;
    }
    if (instance_compile_live || instance_compile_bytes || runtime_live || runtime_bytes ||
        runtime_owned || runtime_owned_capacity) complete = false;
    printf("source-file-deletion-native release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

_Static_assert(sizeof(original_source) - 1u == 402u, "Complete original root and legal Test");
_Static_assert(sizeof(library_source) - 1u == 80u, "Complete original library declaration");

int main(int argc, char **argv) {
    const char *output = NULL;
    if (argc == 6 && !strcmp(argv[4], "--emit")) output = argv[5];
    else if (argc != 4 || !native_spec()) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    DeletionRun run = {0};
    run.context.limits = xr_xir_compile_default_limits();
    run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = phase(&run, "owner-created") && create_owned_sources(&run, argv[1], argv[2], argv[3]);
    if (passed) passed = produce(&run, run.source_root, run.owned_path[0], run.owned_path[1]);
    if (passed) passed = read_packets(&run, run.owned_path[0], run.owned_path[1]) && detach_lower(&run) && emit_owned(&run);
    if (passed) passed = output ? write_translation_unit(&run, output) :
        (seal_native_instances(&run) && execute_instances(&run));
    if (!passed) fprintf(stderr, "source-file-deletion-native failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("source-file-deletion-native mode=%s result=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " canonical=%u Unit=%u permissions=%u expected-answer=42 "
        "host-answer=NOT_EXPOSED removed-owned-source-files=%u deletion-before-detached-Checked=%u source-literals=RETAINED_IN_IMAGE "
        "full-FI=NOT_RUN external-exports=OPEN legacy-Core-identity=OPEN\n",
        output ? "emit" : "native", passed && released ? "PASS" : "FAIL", instance_compile_attempts, runtime_attempts,
        run.final.allocated_bytes, run.final.peak_bytes, run.final.work, run.canonical_count, run.Unit_count, run.permission_count,
        run.removed_count, run.deletion_proved ? 1u : 0u);
    return passed && released ? 0 : 1;
}
