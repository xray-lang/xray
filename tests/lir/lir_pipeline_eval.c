/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_pipeline_eval.c - Owned source pipelines with independent root receipts
 *
 * KEY CONCEPT:
 *   Transformations keep their real program root. Numeric receipts distinguish
 *   whole modules, structural root CFGs and the emitter's function selection.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "base/xmalloc.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_sccp.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source_dependencies.h"
#include "vm/register/xrbc_lir.h"
#include <errno.h>
#include <inttypes.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <time.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

typedef struct Shape {
    uint64_t functions, external, blocks, instructions, values, calls, leaves, self_calls;
    uint64_t types[XR_LIR_TYPE_COUNT];
} Shape;
typedef struct Snapshot {
    const char *name;
    uint32_t root;
    Shape module, cfg, selected;
} Snapshot;
typedef struct Stage { const char *name; uint64_t ticks; } Stage;
typedef struct Sample { uint64_t prepare, run, bits; } Sample;
typedef struct Eval {
    uint64_t frequency;
    Stage stages[9];
    size_t stage_count;
    Snapshot snapshots[5];
    size_t snapshot_count;
    uint32_t comparisons;
    uint32_t rbc_registers, rbc_spills, rbc_reference_spills;
    uint64_t source_retire, lir_retire, execution_retire, program_retire, observations, comparisons_ticks;
    XrtSourceLocation failure_location;
    XrtOutputFailure failure;
    bool output_failure;
} Eval;

static uint64_t clock_frequency(void) {
#if defined(XR_OS_WINDOWS)
    LARGE_INTEGER frequency;
    CHECK(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0);
    return (uint64_t)frequency.QuadPart;
#else
    return UINT64_C(1000000000);
#endif
}
static uint64_t clock_ticks(void) {
#if defined(XR_OS_WINDOWS)
    LARGE_INTEGER counter;
    CHECK(QueryPerformanceCounter(&counter) && counter.QuadPart >= 0);
    return (uint64_t)counter.QuadPart;
#else
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0 && time.tv_sec >= 0 && time.tv_nsec >= 0);
    CHECK((uint64_t)time.tv_sec <= (UINT64_MAX - (uint64_t)time.tv_nsec) / UINT64_C(1000000000));
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
#endif
}
static uint64_t elapsed(uint64_t start) {
    uint64_t end = clock_ticks();
    CHECK(end >= start);
    return end - start;
}
static void stage(Eval *e, const char *name, uint64_t start) {
    CHECK(e->stage_count < sizeof(e->stages) / sizeof(e->stages[0]));
    e->stages[e->stage_count++] = (Stage){name, elapsed(start)};
}
static void *array(size_t count, size_t width) {
    CHECK(width && count <= SIZE_MAX / width);
    void *p = xr_calloc(count, width);
    CHECK(p || !count);
    return p;
}
static bool number(const char *text, uint32_t *out) {
    if (!text[0]) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    unsigned long long n = strtoull(text, NULL, 10);
    if (errno || n > UINT32_MAX) return false;
    *out = (uint32_t)n;
    return true;
}
static uint32_t function_ordinal(const XrLirModule *m, XrLirFunction wanted) {
    CHECK(wanted.module == m);
    for (uint32_t i = 0; i < xr_lir_function_count(m); ++i) {
        XrLirFunction f;
        OK(xr_lir_function_at(m, i, &f));
        if (f.id == wanted.id) return i;
    }
    CHECK(false);
    return 0;
}
static uint32_t block_ordinal(const XrLirModule *m, XrLirFunction f, uint32_t id) {
    XrLirFunctionView v;
    OK(xr_lir_function_view(m, f, &v));
    for (uint32_t b = 0; b < v.block_count; ++b) {
        XrLirBlock block;
        OK(xr_lir_block_at(m, f, b, &block));
        if (block.id == id) return b;
    }
    CHECK(false);
    return 0;
}

static void same_signature_registry(const XrLirModule *a, const XrLirModule *b) {
    CHECK(a != b && xr_lir_function_count(a) == xr_lir_function_count(b));
    for (uint32_t i = 0; i < xr_lir_function_count(a); ++i) {
        XrLirFunction af, bf;
        XrLirFunctionView av, bv;
        OK(xr_lir_function_at(a, i, &af)); OK(xr_lir_function_at(b, i, &bf));
        OK(xr_lir_function_view(a, af, &av)); OK(xr_lir_function_view(b, bf, &bv));
        CHECK(av.kind == bv.kind && av.extern_kind == bv.extern_kind && av.result == bv.result);
        CHECK(av.parameter_count == bv.parameter_count && av.name_length == bv.name_length);
        CHECK(!memcmp(av.name, bv.name, av.name_length));
        for (uint32_t p = 0; p < av.parameter_count; ++p)
            CHECK(av.parameter_types[p] == bv.parameter_types[p]);
    }
    CHECK(xr_lir_intrinsic_count(a) == xr_lir_intrinsic_count(b));
    for (uint32_t i = 0; i < xr_lir_intrinsic_count(a); ++i) {
        XrLirIntrinsic av, bv;
        OK(xr_lir_intrinsic_at(a, i, &av)); OK(xr_lir_intrinsic_at(b, i, &bv));
        CHECK(av.id == bv.id && av.parameter_count == bv.parameter_count &&
            av.result == bv.result && av.scalar_sync == bv.scalar_sync);
        for (uint32_t p = 0; p < av.parameter_count; ++p) CHECK(av.parameters[p] == bv.parameters[p]);
    }
}

typedef struct Walk {
    const XrLirModule *m;
    uint8_t *seen;
    uint32_t *queue, count;
    Shape shape;
} Walk;
static void visit(Walk *w, uint32_t ordinal) {
    CHECK(ordinal < xr_lir_function_count(w->m));
    if (!w->seen[ordinal]) {
        w->seen[ordinal] = 1;
        w->queue[w->count++] = ordinal;
    }
}
static void count_block(Walk *w, XrLirFunction f, XrLirBlock block) {
    XrLirBlockView view;
    OK(xr_lir_block_view(w->m, block, &view));
    ++w->shape.blocks;
    w->shape.instructions += view.instruction_count;
    for (uint32_t p = 0; p < view.parameter_count; ++p) {
        XrLirType type;
        OK(xr_lir_value_type(w->m, (XrLirValue){w->m, f.id, view.parameters[p]}, &type));
        ++w->shape.values; ++w->shape.types[type];
    }
    for (uint32_t i = 0; i < view.instruction_count; ++i) {
        XrLirInstructionView v;
        OK(xr_lir_instruction_view(w->m, block, i, &v));
        if (v.result) { ++w->shape.values; ++w->shape.types[v.type]; }
        if (v.op != XR_LIR_CALL) continue;
        XrLirFunction callee = {w->m, v.callee};
        XrLirFunctionView cv;
        OK(xr_lir_function_view(w->m, callee, &cv));
        if (cv.kind == XR_LIR_FUNCTION_EXTERNAL) ++w->shape.leaves;
        else ++w->shape.calls;
        if (v.callee == f.id) ++w->shape.self_calls;
        visit(w, function_ordinal(w->m, callee));
    }
}
static Shape shape(const XrLirModule *m, XrLirFunction root, unsigned kind) {
    uint32_t n = xr_lir_function_count(m);
    Walk w = {.m = m, .seen = array(n, 1), .queue = array(n, sizeof(uint32_t))};
    if (!kind) for (uint32_t i = 0; i < n; ++i) visit(&w, i);
    else visit(&w, function_ordinal(m, root));
    for (uint32_t at = 0; at < w.count; ++at) {
        XrLirFunction f;
        XrLirFunctionView v;
        OK(xr_lir_function_at(m, w.queue[at], &f)); OK(xr_lir_function_view(m, f, &v));
        if (v.kind == XR_LIR_FUNCTION_EXTERNAL) { ++w.shape.external; continue; }
        ++w.shape.functions;
        w.shape.values += v.parameter_count;
        for (uint32_t p = 0; p < v.parameter_count; ++p) ++w.shape.types[v.parameter_types[p]];
        if (kind != 1) {
            for (uint32_t b = 0; b < v.block_count; ++b) {
                XrLirBlock block; OK(xr_lir_block_at(m, f, b, &block)); count_block(&w, f, block);
            }
            continue;
        }
        uint8_t *seen = array(v.block_count, 1);
        uint32_t *queue = array(v.block_count, sizeof(*queue));
        uint32_t count = 1;
        queue[0] = block_ordinal(m, f, v.entry); seen[queue[0]] = 1;
        for (uint32_t b = 0; b < count; ++b) {
            XrLirBlock block; XrLirBlockView bv; XrLirInstructionView term;
            OK(xr_lir_block_at(m, f, queue[b], &block)); count_block(&w, f, block);
            OK(xr_lir_block_view(m, block, &bv)); CHECK(bv.instruction_count);
            OK(xr_lir_instruction_view(m, block, bv.instruction_count - 1, &term));
            for (uint32_t edge = 0; edge < term.edge_count; ++edge) {
                uint32_t target = block_ordinal(m, f, term.edges[edge].target);
                if (!seen[target]) { seen[target] = 1; queue[count++] = target; }
            }
        }
        xr_free(queue); xr_free(seen);
    }
    xr_free(w.queue); xr_free(w.seen);
    return w.shape;
}
static void snapshot(Eval *e, const char *name, const XrLirLowered *input) {
    uint64_t start = clock_ticks();
    CHECK(e->snapshot_count < sizeof(e->snapshots) / sizeof(e->snapshots[0]));
    Snapshot *s = &e->snapshots[e->snapshot_count++];
    s->name = name; s->root = function_ordinal(input->module, input->entry);
    s->module = shape(input->module, input->entry, 0);
    s->cfg = shape(input->module, input->entry, 1);
    s->selected = shape(input->module, input->entry, 2);
    e->observations += elapsed(start);
}
static void transform(Eval *e, XrLirLowered *current, bool inlining, const char *name) {
    uint32_t ordinal = function_ordinal(current->module, current->entry);
    XrLirModule *next = NULL;
    uint64_t start = clock_ticks();
    OK(inlining ? xr_lir_inline(current->module, &next) : xr_lir_sccp(current->module, &next));
    stage(e, name, start);
    start = clock_ticks();
    same_signature_registry(current->module, next);
    XrLirFunction entry;
    OK(xr_lir_function_at(next, ordinal, &entry));
    XrLirFunctionView view;
    OK(xr_lir_function_view(next, entry, &view));
    CHECK(view.result == XR_LIR_I64 && !view.parameter_count);
    e->comparisons_ticks += elapsed(start);
    start = clock_ticks();
    xr_lir_lowered_free(current);
    e->lir_retire += elapsed(start);
    *current = (XrLirLowered){next, entry};
    ++e->comparisons;
    snapshot(e, name, current);
}

static void check_initialization(const XrXirArtifact *closed, const XrLirLowered *lowered) {
    const XrXirModule *input = xr_xir_compile_artifact_module(closed);
    const XrXirDeclarations *d = input->declarations;
    // Mutable Cells retain their existing activation-owned wrapper contract.
    for (uint32_t s=0; s<d->slot_count; ++s) if (d->slots[s].mutable) return;
    uint32_t *order = array(d->module_count, sizeof(*order)), count = 0;
    uint8_t *active = array(d->module_count, 1);
    CHECK(xr_xir_compile_declarations_active(xr_xir_compile_artifact_context(closed),
        d, order, active, &count) == XR_XIR_OK);
    XrLirBlock block;
    XrLirBlockView view;
    OK(xr_lir_block_at(lowered->module, lowered->entry, 0, &block));
    OK(xr_lir_block_view(lowered->module, block, &view));
    CHECK(view.instruction_count == count + 2);
    for (uint32_t i=0; i<=count; ++i) {
        uint32_t selected = i == count ? d->entry_function : d->modules[order[i]].initializer;
        XrLirInstructionView call;
        XrLirFunctionView callee;
        OK(xr_lir_instruction_view(lowered->module, block, i, &call));
        CHECK(call.op == XR_LIR_CALL);
        OK(xr_lir_function_view(lowered->module,
            (XrLirFunction){lowered->module, call.callee}, &callee));
        char prefix[32];
        int length = snprintf(prefix, sizeof(prefix), "f%u_", selected);
        CHECK(length > 0 && (size_t)length < sizeof(prefix) &&
            callee.name_length == (size_t)length + input->functions[selected].name_length &&
            !memcmp(callee.name, prefix, (size_t)length) &&
            !memcmp(callee.name + length, input->functions[selected].name,
                    input->functions[selected].name_length));
    }
    xr_free(active); xr_free(order);
}

static XrLirLowered source_lower(Eval *e, const char *path, XrCompileResources *resources,
                               char **expected_path, const char *stdlib) {
    XrCompilerSession *session = NULL;
    XrModuleIdentityAuthority authority = {0}; char *authority_root = NULL;
    XrXirSourceResult source = {0}; XrXirArtifact *closed = NULL;
    XrXirSourceDiagnostic diagnostic = {0}; XrXirSourceFailure failure = {0};
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_module_identity_script_authority_from_source(resources, path,
        &authority, &authority_root) == XR_MODULE_OK);
    XrXirCompileContext context = {.resources = resources, .limits = xr_xir_compile_default_limits()};
    XrXirSourceRequest request = {.session = session, .entry_path = path, .authority = &authority,
        .context = &context, .linkage_kind = XR_XIR_PROGRAM, .stdlib_path = stdlib};
    uint64_t start = clock_ticks();
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &diagnostic, &failure);
    if (status != XR_XIR_OK) fprintf(stderr, "source: %s\n", diagnostic.message);
    CHECK(status == XR_XIR_OK); stage(e, "source_check", start);
    start = clock_ticks();
    CHECK(xr_xir_compile_specialize(source.checked, &closed, NULL, NULL) == XR_XIR_OK);
    stage(e, "specialize", start);
    XrLirLowered lowered = {0}; XrLirLowerFailure lower_failure = {0};
    XrLirLowerRequest lower = {closed, source.snapshot, false};
    start = clock_ticks();
    XrLirStatus ls = xr_lir_lower_program_checked(&lower, &lowered, &lower_failure);
    if (ls != XR_LIR_OK) fprintf(stderr, "lower: %s\n", lower_failure.diagnostic.message);
    CHECK(ls == XR_LIR_OK); stage(e, "lower", start);
    check_initialization(closed, &lowered);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(source.snapshot);
    const XrXirSourceDependencies *dependencies =
        xr_xir_compile_source_snapshot_dependencies(source.snapshot);
    CHECK(view && dependencies && dependencies->entry < dependencies->count);
    uint32_t root_module = dependencies->entries[dependencies->entry].module;
    CHECK(root_module < view->module_count);
    const XrXirSourceQueryModule *root = &view->modules[root_module];
    const char *borrowed = root->path ? root->path : root->identity;
    CHECK(borrowed && strlen(borrowed) < SIZE_MAX);
    *expected_path = xr_malloc(strlen(borrowed) + 1); CHECK(*expected_path);
    memcpy(*expected_path, borrowed, strlen(borrowed) + 1);
    start = clock_ticks();
    xr_lir_lower_failure_free(&lower_failure);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_failure_free(&failure);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session); xr_compile_resources_free(authority_root);
    e->source_retire += elapsed(start);
    return lowered;
}
static void write_bytes(const char *path, const void *bytes, size_t count) {
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(bytes, 1, count, file) == count);
    CHECK(fclose(file) == 0);
}
static void shape_json(FILE *f, const Shape *s) {
    fprintf(f, "{\"functions\":%" PRIu64 ",\"external\":%" PRIu64
        ",\"blocks\":%" PRIu64 ",\"instructions\":%" PRIu64 ",\"values\":%" PRIu64
        ",\"ordinary_calls\":%" PRIu64 ",\"leaf_calls\":%" PRIu64 ",\"self_calls\":%" PRIu64
        ",\"type_definitions\":[", s->functions, s->external, s->blocks, s->instructions,
        s->values, s->calls, s->leaves, s->self_calls);
    for (unsigned i = 0; i < XR_LIR_TYPE_COUNT; ++i)
        fprintf(f, "%s%" PRIu64, i ? "," : "", s->types[i]);
    fputs("]}", f);
}
static void receipt(const char *path, const Eval *e, const Sample *samples, uint32_t warmups,
                    uint32_t runs, size_t c_bytes) {
    FILE *f = fopen(path, "wb"); CHECK(f);
    fprintf(f, "{\"clock_frequency\":%" PRIu64 ",\"comparisons\":%u,\"c_bytes\":%zu,"
        "\"root_result_type\":\"i64\",\"warmups\":%u,\"runs\":%u,\"stages\":[",
        e->frequency, e->comparisons, c_bytes, warmups, runs);
    for (size_t i = 0; i < e->stage_count; ++i)
        fprintf(f, "%s{\"name\":\"%s\",\"ticks\":%" PRIu64 "}", i ? "," : "",
            e->stages[i].name, e->stages[i].ticks);
    fputs("],\"snapshots\":[", f);
    for (size_t i = 0; i < e->snapshot_count; ++i) {
        const Snapshot *s = &e->snapshots[i];
        fprintf(f, "%s{\"name\":\"%s\",\"root_ordinal\":%u,\"MODULE\":", i ? "," : "", s->name, s->root);
        shape_json(f, &s->module); fputs(",\"ROOT_CFG_CLOSURE\":", f); shape_json(f, &s->cfg);
        fputs(",\"C_SELECTED_FUNCTION_ALL_BLOCKS\":", f); shape_json(f, &s->selected); fputc('}', f);
    }
    fprintf(f,"],\"rbc_registers_max\":%u,\"rbc_spills\":%u,\"rbc_reference_spills\":%u,\"samples\":[",
        e->rbc_registers,e->rbc_spills,e->rbc_reference_spills);
    for (uint64_t i = 0; i < (uint64_t)warmups + runs; ++i) {
        fprintf(f, "%s{\"warmup\":%s,\"prepare_ticks\":%" PRIu64 ",\"run_ticks\":%" PRIu64
            ",\"completed_entry\":%s,\"bits\":", i ? "," : "", i < warmups ? "true" : "false",
            samples[i].prepare, samples[i].run, e->output_failure ? "false" : "true");
        if (e->output_failure) fputs("null}", f);
        else fprintf(f, "%" PRIu64 "}", samples[i].bits);
    }
    fprintf(f, "],\"retirement_ticks\":{\"source_inputs\":%" PRIu64 ",\"lir_modules\":%" PRIu64
        ",\"executions_outcomes\":%" PRIu64 ",\"program\":%" PRIu64 "},"
        "\"observation_ticks\":%" PRIu64 ",\"comparison_ticks\":%" PRIu64,
        e->source_retire, e->lir_retire, e->execution_retire, e->program_retire,
        e->observations, e->comparisons_ticks);
    fprintf(f, ",\"status\":\"%s\",\"host_continued\":%s,\"failure_location\":[%u,%u,%u,%u],"
        "\"owner_path_matches_original\":%s,\"failure_facts\":{\"domain\":%u,\"code\":%u,"
        "\"operation\":%u,\"source_id\":%u,\"broken_pipe\":%s},\"formal_performance\":false}\n",
        e->output_failure ? "XR_RBC_OUTPUT_FAILURE" : "XR_RBC_OK",
        e->output_failure ? "true" : "false", e->failure_location.line, e->failure_location.column,
        e->failure_location.end_line, e->failure_location.end_column, e->output_failure ? "true" : "false",
        (unsigned)e->failure.io.domain, (unsigned)e->failure.io.code, (unsigned)e->failure.operation,
        e->failure.source_id, e->failure.io.broken_pipe ? "true" : "false");
    CHECK(!ferror(f) && fclose(f) == 0);
}
static void program_free(Eval *e, XrRbcProgram **program) {
    if (!*program) return;
    uint64_t start = clock_ticks();
    xr_rbc_program_destroy(*program); *program = NULL;
    e->program_retire += elapsed(start);
}
static void run_vm(Eval *e, XrRbcProgram **program, Sample *sample, const char *expected,
                   unsigned failure_order) {
    XrRbcExecution *execution = NULL;
    uint64_t start = clock_ticks();
    CHECK(xr_rbc_program_prepare(*program, NULL, 0, &execution) == XR_RBC_OK && execution);
    sample->prepare = elapsed(start);
    start = clock_ticks();
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution); CHECK(outcome);
    sample->run = elapsed(start);
    CHECK(!xr_rbc_execution_run(execution));
    start = clock_ticks(); xr_rbc_execution_free(&execution); e->execution_retire += elapsed(start);
    if (failure_order == 1) program_free(e, program);
    if (!failure_order) {
        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK);
        CHECK(xr_rbc_outcome_bits(outcome, &sample->bits) && sample->bits == 0);
    } else {
        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OUTPUT_FAILURE);
        uint64_t missing_bits = UINT64_MAX;
        CHECK(!xr_rbc_outcome_bits(outcome, &missing_bits) && missing_bits == UINT64_MAX);
        XrtOutputFailure facts = {0}; const XrtLocationOwner *owner = NULL;
        CHECK(xr_rbc_outcome_output_failure(outcome, &facts, &owner));
        CHECK(facts.io.failed && facts.io.code && facts.io.domain != XRT_IO_NONE &&
            facts.operation == XRT_OUTPUT_WRITE && !facts.io.broken_pipe);
        const XrtSourceLocation *location = xrt_locations_get(owner, facts.source_id);
        CHECK(location && location->path_length == strlen(expected));
        CHECK(!memcmp(location->path, expected, location->path_length));
        e->failure_location = *location;
        e->failure_location.path = NULL; e->failure_location.path_length = 0;
        e->failure = facts;
        e->output_failure = true;
    }
    start = clock_ticks(); xr_rbc_outcome_free(&outcome); e->execution_retire += elapsed(start);
    if (failure_order == 2) program_free(e, program);
}

int main(int argc, char **argv) {
    if (argc != 8 && argc != 9) {
        fputs("usage: lir_pipeline_eval <source> <mode> <output.c> <receipt.json> <warmups> <runs> <normal|program-first|outcome-first> [stdlib-directory]\n", stderr);
        return 2;
    }
    const char *mode = argv[2];
    bool raw = !strcmp(mode, "raw"), sccp = !strcmp(mode, "sccp"), inlining = !strcmp(mode, "inline");
    bool combined = !strcmp(mode, "sccp-inline-inline-sccp");
    if (!raw && !sccp && !inlining && !combined) return 2;
    unsigned failure_order = !strcmp(argv[7], "normal") ? 0u :
        !strcmp(argv[7], "program-first") ? 1u : !strcmp(argv[7], "outcome-first") ? 2u : 3u;
    if (failure_order == 3) return 2;
    uint32_t warmups = 0, runs = 0;
    if (!number(argv[5], &warmups) || !number(argv[6], &runs) || !runs ||
        (failure_order && (warmups || runs != 1))) return 2;
    uint64_t count = (uint64_t)warmups + runs; CHECK(count <= SIZE_MAX);
    Sample *samples = array((size_t)count, sizeof(*samples));
    Eval e = {.frequency = clock_frequency()};
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    char *expected_path = NULL;
    XrLirLowered lowered = source_lower(&e, argv[1], resources, &expected_path, argc == 9 ? argv[8] : NULL);
    snapshot(&e, "raw", &lowered);
    if (sccp || combined) transform(&e, &lowered, false, "sccp_1");
    if (inlining || combined) transform(&e, &lowered, true, "inline_1");
    if (combined) { transform(&e, &lowered, true, "inline_2"); transform(&e, &lowered, false, "sccp_2"); }
    XrLirFunctionView root;
    OK(xr_lir_function_view(lowered.module, lowered.entry, &root));
    CHECK(root.result == XR_LIR_I64 && !root.parameter_count);
    XrLirCRequest request = {lowered.module, lowered.entry, resources, argv[1], NULL};
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic = {0};
    uint64_t start = clock_ticks();
    CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_OK);
    CHECK(output.entry_result == XR_LIR_I64); stage(&e, "emit_c_with_verifier", start);
    XrRbcProgram *program = NULL; XrRbcCompileDiagnostic bd = {0};
    start = clock_ticks();
    XrRbcCompileStatus bs = xr_rbc_compile(lowered.module, lowered.entry, &program, &bd);
    if (bs != XR_RBC_COMPILE_OK) fprintf(stderr, "bytecode: %s\n", bd.message);
    CHECK(bs == XR_RBC_COMPILE_OK && program);
    const XrRbcModule *rbc=xr_rbc_program_view(program);
    for(size_t f=0;f<rbc->function_count;++f) {
        const XrRbcFunction *body=&rbc->functions[f];
        if(e.rbc_registers<body->register_count)e.rbc_registers=body->register_count;
        e.rbc_spills+=body->spill_count;e.rbc_reference_spills+=body->reference_spill_count;
    }
    stage(&e, "rbc_compile", start);
    start = clock_ticks(); xr_lir_lowered_free(&lowered); e.lir_retire += elapsed(start);
    write_bytes(argv[3], output.bytes, output.length);
    for (uint64_t i = 0; i < count; ++i) run_vm(&e, &program, &samples[i], expected_path, failure_order);
    program_free(&e, &program);
    receipt(argv[4], &e, samples, warmups, runs, output.length);
    xr_lir_c_output_free(&output); xr_compile_resources_release(resources);
    xr_free(expected_path); xr_free(samples);
    if (failure_order) fputs("HOST_ALIVE\n", stderr);
    return 0;
}
