/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_cancel_prefix.c - Measured cancellation of authentic activations
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xplatform.h"
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_fault_cleanup_fixture.h"
static const unsigned prefix_counts[3][2] = {{18, 17}, {24, 21}, {25, 22}};
enum { PREFIX_MEASURE, PREFIX_CANCEL, PREFIX_STOP, PREFIX_BUSY, PREFIX_BEFORE, PREFIX_CLEANUP, PREFIX_AFTER };
typedef struct PrefixOutput {
    XrXirInstance *instance;
    int64_t trace[32];
    unsigned count, action, accepted, busy, child_requests, root_requests;
    bool error, rejected;
} PrefixOutput;
static XrXirOutputStatus prefix_output(void *context, const XrXirOutputGroup *group) {
    PrefixOutput *output = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && output->count < 32);
    int64_t value = group->values[0].payload;
    CHECK(value == 7 || value == 6 || value == 5);
    output->trace[output->count++] = value;
    if (output->action == PREFIX_BUSY && value == 7) {
        CHECK(xr_xir_instance_cancel_current(output->instance) == XR_XIR_CALL_BUSY);
        ++output->busy;
    }
    if (!output->accepted && ((output->action == PREFIX_BEFORE && value == 7) ||
            (output->action == PREFIX_CLEANUP && value == 6))) {
        XirTaskActivation *activation = output->instance->executor->current;
        CHECK(activation && activation->call && activation->call->driving);
        CHECK(xr_xir_call_request_cancel(activation->call) == XR_XIR_CALL_CANCEL_REQUESTED);
        ++output->accepted;
        if (activation->task) ++output->child_requests; else ++output->root_requests;
    }
    if (output->error && value == 7 && !output->rejected) {
        output->rejected = true;
        return XR_XIR_OUTPUT_ERROR;
    }
    return XR_XIR_OUTPUT_OK;
}
/* Only the root result and the script entry differ from the immutable graph.
 * The returned handle is the actual GO result, with the Program's own arena. */
static XrXirProgram *prefix_program(unsigned mode) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    TaskFaultFixture f; task_fault_fixture(&f, mode);
    XrXirFunction functions[6]; XrXirFunctionIdentity identities[6];
    memcpy(functions, f.functions, sizeof(f.functions)); memcpy(identities, f.identities, sizeof(f.identities));
    identities[5] = (XrXirFunctionIdentity){0};
    XrXirInstruction script[2] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64}, {.op = XR_XIR_RETURN}};
    XrXirBlock script_block = {.count = 2};
    functions[5] = (XrXirFunction){.name = "script", .name_length = 6, .result = XR_XIR_I64,
        .blocks = &script_block, .block_count = 1, .instructions = script, .instruction_count = 2};
    if (mode != TASK_FAULT_CALL) {
        functions[0].result = (XrXirType)256;
        f.root[mode == TASK_FAULT_BODY ? 2 : 5].args[0] = mode == TASK_FAULT_BODY ? 1 : 2;
    }
    f.declarations.functions = identities; f.declarations.entry_function = 5;
    f.module.functions = functions; f.module.function_count = 6;
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(context, &f.module, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "mode=%u check=%u function=%u op=%u\n", mode, status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); memset(&f, 0xa5, sizeof(f));
    memset(functions, 0xa5, sizeof(functions)); memset(identities, 0xa5, sizeof(identities));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    CHECK(mode != TASK_FAULT_CALL || (!program->arena && !program->types));
    return program;
}
static XrXirCall *prefix_fault_call(XrXirInstance *instance) {
    if (instance->call && instance->call->abort_reason == XR_XIR_CALL_OUTPUT_ERROR &&
        xr_xir_call_state(instance->call) == XR_XIR_CALL_READY) return instance->call;
    for (XirTask *task = instance->executor->active; task; task = task->active_next)
        if (task->call && task->call->abort_reason == XR_XIR_CALL_OUTPUT_ERROR &&
            xr_xir_call_state(task->call) == XR_XIR_CALL_READY) return task->call;
    return NULL;
}
static unsigned prefix_obligations(const XrXirCall *call) {
    unsigned mask = 0;
    if (call) for (const CallFrame *frame = call->top; frame; frame = frame->parent) {
        if (frame->cleanup_frontier == 1) mask |= 1;
        if (frame->cleanup_frontier == 2) mask |= 2;
    }
    return mask;
}
static unsigned prefix_trace_mask(const PrefixOutput *output) {
    unsigned mask = 0;
    for (unsigned i = 0; i < output->count; ++i) {
        unsigned bit = output->trace[i] == 6 ? 1u : output->trace[i] == 5 ? 2u : 4u;
        CHECK(!(mask & bit));
        if (bit == 2 && (mask & 1)) CHECK(i && output->trace[i - 1] == 6);
        mask |= bit;
    }
    return mask;
}
static void prefix_row(const XrXirInstance *instance, const PrefixOutput *output, unsigned mode,
    unsigned index, unsigned prefix, XrXirCallStatus public_status) {
    unsigned active = 0, child_frontier = 0, child_pending = 0;
    for (XirTask *task = instance->executor->active; task; task = task->active_next) {
        ++active;
        if (task->call && task->call->top) child_frontier += task->call->top->cleanup_frontier;
        if (task->call && task->call->abort_reason == XR_XIR_CALL_OUTPUT_ERROR) ++child_pending;
    }
    XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(instance->domain);
    printf("{\"row\":\"prefix\",\"mode\":%u,\"instance\":%u,\"prefix\":%u,\"status\":%u,\"root_status\":%u,"
        "\"root_frontier\":%u,\"active_children\":%u,\"child_frontier\":%u,\"pending13\":%u,\"outputs\":%u,"
        "\"attempts\":%zu,\"requested_value\":%llu,\"requested_call\":%llu,\"work\":%llu}\n",
        mode, index, prefix, public_status, xr_xir_call_state(instance->call),
        instance->call->top ? instance->call->top->cleanup_frontier : 0, active, child_frontier,
        child_pending + (instance->call->abort_reason == XR_XIR_CALL_OUTPUT_ERROR), output->count,
        runtime_attempts, (unsigned long long)budget.requested_bytes,
        (unsigned long long)budget.requested_call_bytes, (unsigned long long)budget.work);
}
static XrXirInstanceResult prefix_drain(XrXirInstance *instance, unsigned *polls) {
    XrXirInstanceResult result = {0};
    do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++*polls < 3000); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    CHECK(result.outcome.status != XR_XIR_CALL_SUSPENDED);
    return result;
}
static void prefix_trace(const PrefixOutput *output, unsigned mode) {
    CHECK(output->count == (mode == TASK_FAULT_BODY ? 2u : 3u));
    CHECK(output->trace[0] == 7 && output->trace[1] == 6);
    CHECK(mode == TASK_FAULT_BODY || output->trace[2] == 5);
}
static XrXirValue prefix_take(XrXirInstance *instance, unsigned mode, XrXirCallStatus status) {
    XrXirValue value = {0};
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        if (mode == TASK_FAULT_CALL) CHECK(value.type == XR_XIR_I64 && value.payload == 7);
        else CHECK(value.type == 256 && xr_xir_value_arena(&value) == instance->program->arena);
    }
    return value;
}
static void prefix_escape(XrXirValue *value, unsigned mode, XrXirCallStatus task_status) {
    if (!value->type) return;
    XrXirValue copy = {0}; CHECK(xr_xir_value_copy(value, &copy) == XR_XIR_VALUE_OK);
    if (mode != TASK_FAULT_CALL) {
        XrXirCallResult outcome = {0}, repeat = {0};
        CHECK(xr_xir_task_copy_outcome(value, &outcome) == task_status);
        CHECK(xr_xir_task_copy_outcome(&copy, &repeat) == task_status);
        if (task_status == XR_XIR_CALL_RETURNED) CHECK(outcome.value.type == XR_XIR_I64 && outcome.value.payload == 7 && repeat.value.payload == 7);
        xr_xir_call_result_drop(&outcome); xr_xir_call_result_drop(&repeat);
    } else CHECK(copy.type == XR_XIR_I64 && copy.payload == 7);
    xr_xir_value_drop(&copy); xr_xir_value_drop(value);
}
static void prefix_execute(unsigned mode, bool error, unsigned action, unsigned selected) {
    XrXirProgram *program = prefix_program(mode);
    XrXirInstance *instances[2] = {0}; PrefixOutput outputs[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(config.requested_value_limit == UINT64_C(67108864) && config.requested_call_limit == UINT64_C(67108864) &&
            config.work_limit == UINT64_C(128000000));
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, prefix_output, &outputs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        outputs[i].instance = instances[i];
    }
    CHECK(instances[0]->domain != instances[1]->domain && instances[0]->executor != instances[1]->executor);
    xr_xir_compile_program_drop(program);
    XrXirValue warm[2] = {{0}}, escaped[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = instances[i]; PrefixOutput *output = &outputs[i]; unsigned polls = 0;
        CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = prefix_drain(instance, &polls);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && instance->state == XR_XIR_INSTANCE_READY);
        prefix_trace(output, mode); warm[i] = prefix_take(instance, mode, result.outcome.status);
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
        uint64_t epoch = instance->epoch; *output = (PrefixOutput){.instance = instance, .error = error, .action = action};
        CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY && instance->epoch == epoch + 1);
        polls = 0; bool requested = false; XrXirCallStatus request = XR_XIR_CALL_READY;
        XrXirCallStatus requested_root = XR_XIR_CALL_READY;
        size_t attempts_at_request = 0; unsigned obligations = 0; bool accepted_fault = false;
        for (;;) {
            if (action == PREFIX_MEASURE) prefix_row(instance, output, mode, i, polls, XR_XIR_CALL_READY);
            if (!requested && (action == PREFIX_CANCEL || action == PREFIX_STOP) && polls == selected) {
                obligations = prefix_obligations(instance->call);
                accepted_fault = instance->call->abort_reason == XR_XIR_CALL_OUTPUT_ERROR;
                for (XirTask *task = instance->executor->active; task; task = task->active_next) {
                    obligations |= prefix_obligations(task->call);
                    accepted_fault |= task->call->abort_reason == XR_XIR_CALL_OUTPUT_ERROR;
                }
                XrXirCallStatus prior_root = xr_xir_call_state(instance->call);
                requested_root = prior_root;
                request = action == PREFIX_CANCEL ? xr_xir_instance_cancel_current(instance) : xr_xir_instance_stop(instance);
                if (action == PREFIX_CANCEL) CHECK(request ==
                    (prior_root == XR_XIR_CALL_READY || prior_root == XR_XIR_CALL_SUSPENDED ?
                        XR_XIR_CALL_CANCEL_REQUESTED : XR_XIR_CALL_BAD_STATE));
                CHECK(action != PREFIX_CANCEL || request == XR_XIR_CALL_CANCEL_REQUESTED || request == XR_XIR_CALL_BAD_STATE);
                CHECK(action != PREFIX_STOP || request == XR_XIR_CALL_READY || request == XR_XIR_CALL_OUTPUT_ERROR ||
                    (request == XR_XIR_CALL_RETURNED && prior_root == XR_XIR_CALL_RETURNED));
                requested = true; attempts_at_request = runtime_attempts;
            }
            if (!requested && action == PREFIX_AFTER) {
                XrXirCall *fault = prefix_fault_call(instance);
                if (fault) {
                    request = xr_xir_call_request_cancel(fault); CHECK(request == XR_XIR_CALL_CANCEL_REQUESTED);
                    requested = true; attempts_at_request = runtime_attempts;
                    if (fault != instance->call) ++output->child_requests; else ++output->root_requests;
                }
            }
            result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 3000);
            if (result.outcome.status != XR_XIR_CALL_READY) break;
        }
        CHECK(result.outcome.status != XR_XIR_CALL_SUSPENDED);
        if (action == PREFIX_MEASURE) CHECK(polls == prefix_counts[mode][error]);
        if (action == PREFIX_MEASURE || action == PREFIX_BUSY || action == PREFIX_AFTER) {
            CHECK(result.outcome.status == (error ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_RETURNED));
            prefix_trace(output, mode);
        }
        if (action == PREFIX_BUSY) CHECK(output->busy == 1 && !output->accepted);
        if (action == PREFIX_BEFORE || action == PREFIX_CLEANUP) {
            CHECK(output->accepted == 1);
            CHECK(result.outcome.status == (mode == TASK_FAULT_BODY ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED));
            if (action == PREFIX_BEFORE) CHECK(output->count == (mode == TASK_FAULT_BODY ? 2u : 3u));
        }
        if (action == PREFIX_AFTER) CHECK(requested && request == XR_XIR_CALL_CANCEL_REQUESTED);
        if (action == PREFIX_CANCEL || action == PREFIX_STOP) {
            CHECK(requested);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED || result.outcome.status == XR_XIR_CALL_CANCELLED ||
                result.outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK((prefix_trace_mask(output) & obligations) == obligations);
            if (accepted_fault) CHECK(result.outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
        }
        CHECK(!instance->executor->cleanup_incomplete && !xr_xir_call_cleanup_incomplete(instance->call));
        escaped[i] = prefix_take(instance, mode, result.outcome.status);
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
        CHECK(after.bound && !instance->call->budget->exhausted && after.requested_bytes >= before.requested_bytes &&
            after.requested_call_bytes > before.requested_call_bytes && after.work > before.work);
        printf("{\"row\":\"terminal\",\"mode\":%u,\"instance\":%u,\"error\":%u,\"action\":%u,\"selected\":%u,"
            "\"prefix_count\":%u,\"status\":%u,\"request\":%u,\"request_root\":%u,\"callback_accepted\":%u,\"busy\":%u,"
            "\"root_requests\":%u,\"child_requests\":%u,\"obligations\":%u,\"accepted_fault\":%u,\"outputs\":%u,\"request_attempts\":%zu,\"drain_attempts\":%zu,"
            "\"before_value\":%llu,\"before_call\":%llu,\"before_work\":%llu,"
            "\"requested_value\":%llu,\"requested_call\":%llu,\"work\":%llu,\"epoch\":%llu}\n",
            mode, i, error, action, selected, polls, result.outcome.status, request, requested_root, output->accepted, output->busy,
            output->root_requests, output->child_requests, obligations, accepted_fault, output->count, attempts_at_request, runtime_attempts,
            (unsigned long long)before.requested_bytes, (unsigned long long)before.requested_call_bytes,
            (unsigned long long)before.work,
            (unsigned long long)after.requested_bytes, (unsigned long long)after.requested_call_bytes,
            (unsigned long long)after.work, (unsigned long long)instance->epoch);
        XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
        XrXirCallStatus freed = xr_xir_instance_free(instance);
        CHECK(freed == XR_XIR_CALL_READY || (error && freed == XR_XIR_CALL_OUTPUT_ERROR));
        prefix_escape(&warm[i], mode, XR_XIR_CALL_RETURNED);
        if (escaped[i].type) {
            XrXirCallStatus expected_task = XR_XIR_CALL_RETURNED;
            if (action == PREFIX_BEFORE || action == PREFIX_CLEANUP) expected_task = XR_XIR_CALL_CANCELLED;
            if (action == PREFIX_STOP && mode == TASK_FAULT_BODY) expected_task = XR_XIR_CALL_CANCELLED;
            prefix_escape(&escaped[i], mode, expected_task);
        }
        XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(domain);
        CHECK(final.bound && !final.metadata_live && !final.call_live &&
            final.requested_bytes >= after.requested_bytes && final.requested_call_bytes >= after.requested_call_bytes && final.work >= after.work);
        printf("{\"row\":\"disposed\",\"mode\":%u,\"instance\":%u,\"free_status\":%u,\"attempts\":%zu,"
            "\"requested_value\":%llu,\"requested_call\":%llu,\"work\":%llu,\"metadata_live\":0,\"call_live\":0}\n",
            mode, i, freed, runtime_attempts, (unsigned long long)final.requested_bytes,
            (unsigned long long)final.requested_call_bytes, (unsigned long long)final.work);
        xr_xir_domain_drop(domain);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("{\"row\":\"physical\",\"runtime_blocks\":0,\"runtime_bytes\":0,\"instances\":2}\n");
}
static void prefix_process(const char *phase) {
#if defined(XR_OS_WINDOWS)
    FILETIME created, exited, kernel, user, now;
    CHECK(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    GetSystemTimePreciseAsFileTime(&now);
    uint64_t creation = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    uint64_t timestamp = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    printf("{\"row\":\"process\",\"phase\":\"%s\",\"pid\":%lu,\"creation_filetime\":%llu,\"timestamp_filetime\":%llu}\n",
        phase, (unsigned long)GetCurrentProcessId(), (unsigned long long)creation, (unsigned long long)timestamp);
#else
    printf("{\"row\":\"process\",\"phase\":\"%s\",\"pid_observation\":\"WINDOWS_ONLY\"}\n", phase);
#endif
}
static bool prefix_number(const char *text, unsigned *value) {
    if (!text || !*text) return false;
    unsigned result = 0;
    for (; *text; ++text) { if (*text < '0' || *text > '9' || result > 299u) return false; result = result * 10 + (unsigned)(*text - '0'); }
    if (result >= 3000) return false;
    *value = result; return true;
}
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    unsigned mode = !strcmp(argv[2], "body") ? TASK_FAULT_BODY : !strcmp(argv[2], "call") ? TASK_FAULT_CALL :
        !strcmp(argv[2], "await") ? TASK_FAULT_AWAIT : TASK_FAULT_CASES;
    bool error = !strcmp(argv[3], "error");
    if (mode == TASK_FAULT_CASES || (!error && strcmp(argv[3], "ok"))) return 2;
    unsigned action = PREFIX_MEASURE, selected = 0;
    if (!strcmp(argv[1], "--measure")) { if (argc != 4) return 2; }
    else if (!strcmp(argv[1], "--control")) {
        if (argc != 5) return 2;
        action = !strcmp(argv[4], "busy") ? PREFIX_BUSY : !strcmp(argv[4], "before") ? PREFIX_BEFORE :
            !strcmp(argv[4], "cleanup") ? PREFIX_CLEANUP : !strcmp(argv[4], "after") ? PREFIX_AFTER : PREFIX_MEASURE;
        if (action == PREFIX_MEASURE || (action == PREFIX_AFTER && !error) || (action == PREFIX_BEFORE && !error) ||
            (action == PREFIX_CLEANUP && error)) return 2;
    } else if (!strcmp(argv[1], "--prefix")) {
        if (argc != 6 || !prefix_number(argv[5], &selected)) return 2;
        action = !strcmp(argv[4], "cancel") ? PREFIX_CANCEL : !strcmp(argv[4], "stop") ? PREFIX_STOP : PREFIX_MEASURE;
        if (action == PREFIX_MEASURE || selected >= prefix_counts[mode][error]) return 2;
    } else return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    prefix_process("start"); prefix_execute(mode, error, action, selected); effects_source_owners_free();
    prefix_process("end"); return 0;
}
