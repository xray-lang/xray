/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_language_reference.c - Independent generic and optional source expectations
 *
 * KEY CONCEPT:
 *   A definition error cannot be repaired by a concrete caller, and an owned
 *   Checked packet must preserve exact optional layers after its source dies.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_construction.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"

static uint32_t answer_entry(const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 15 && !memcmp(function->name, "referenceAnswer", 15)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    }
    CHECK(entry != UINT32_MAX); return entry;
}

static void initialization_authority(SourceFixtureOwner *owner, const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirNominalTable *original = module->types->nominals;
    CHECK(original && original->declarations && original->count == 2);
    XrXirNominalDeclaration records[2] = {original->declarations[0], original->declarations[1]};
    uint32_t box = UINT32_MAX, plain = UINT32_MAX;
    for (uint32_t n = 0; n < 2; ++n) {
        if (records[n].name.length == 3 && !memcmp(records[n].name.bytes, "Box", 3)) box = n;
        if (records[n].name.length == 5 && !memcmp(records[n].name.bytes, "Plain", 5)) plain = n;
    }
    CHECK(box < 2 && plain < 2 && box != plain);
    CHECK(records[box].flags == XR_XIR_NOMINAL_MODULE_LITERAL && !records[plain].flags);
    uint32_t caller = answer_entry(artifact);
    CHECK(xr_xir_compile_nominal_access(&owner->context, module, caller, box, 0, XR_XIR_NOMINAL_CONSTRUCT) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&owner->context, module, caller, plain, 0, XR_XIR_NOMINAL_CONSTRUCT) == XR_XIR_OK);
    XrXirNominalTable table = {records, 2, NULL};
    XrXirTypes types = *module->types; types.nominals = &table;
    XrXirModule forged = *module; forged.types = &types;
    records[box].flags = 0;
    CHECK(xr_xir_compile_verify_v2(&owner->context, &forged, xr_xir_compile_artifact_construction(artifact), NULL) == XR_XIR_BAD_STRUCTURE);
    records[box].flags = XR_XIR_NOMINAL_MODULE_LITERAL;
    records[plain].flags = XR_XIR_NOMINAL_MODULE_LITERAL;
    CHECK(xr_xir_compile_verify_v2(&owner->context, &forged, xr_xir_compile_artifact_construction(artifact), NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(original->declarations[box].flags == XR_XIR_NOMINAL_MODULE_LITERAL && !original->declarations[plain].flags);
}

static void execute_answer(XrXirProgram *program, uint32_t entry, int64_t expected, unsigned suspensions) {
    XrXirInstance *instances[2] = {NULL, NULL}; XrXirValue results[2] = {{0}, {0}};
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        unsigned resumed=0;
        XrXirInstanceResult observed=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        while (observed.outcome.status==XR_XIR_CALL_SUSPENDED) {
            CHECK(resumed++<suspensions);
            CHECK(xr_xir_instance_resume(instances[i],observed.epoch,observed.outcome.wake)==XR_XIR_CALL_READY);
            observed=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        }
        CHECK(resumed==suspensions);
        XrXirCallResult outcome=observed.outcome;
        if (outcome.status != XR_XIR_CALL_RETURNED) fprintf(stderr, "instance=%u outcome=%u\n", i, (unsigned)outcome.status);
        CHECK(outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (uint32_t i = 0; i < 2; ++i) {
        if (results[i].payload != expected) fprintf(stderr, "expected=%lld actual=%lld\n", (long long)expected, (long long)results[i].payload);
        CHECK(results[i].type == XR_XIR_I64 && results[i].payload == expected);
        xr_xir_value_drop(&results[i]);
    }
}

static void accept_packet(SourceFixtureOwner *owner, XrXirSourceResult *source, XrCompilerSession *session, int64_t expected, const char *output, unsigned suspensions) {
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(source->checked, &packet, NULL) == XR_XIR_OK);
    XrXirSourceSnapshot *record_snapshot=NULL;
    const XrXirSourceView *source_view=xr_xir_compile_source_snapshot_view(source->snapshot);
    for (uint32_t t=0;source_view->types && t<source_view->types->count;++t)
        if (source_view->types->nodes[t].kind==XR_XIR_TYPE_RECORD) {record_snapshot=source->snapshot;source->snapshot=NULL;break;}
    /* No source arena, AST or session may supply the reconstructed consumer. */
    xr_xir_compile_source_result_free(source); xr_compile_session_free(session);
    if (record_snapshot) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(record_snapshot);
        CHECK(xr_xir_compile_types_structure_verify(&owner->context,view->types)==XR_XIR_OK);
        xr_xir_compile_source_snapshot_free(record_snapshot);
    }
    XrXirArtifact *decoded = NULL, *specialized = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&owner->context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_specialize(decoded, &specialized, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "specialize=%u function=%u instruction=%u\n", (unsigned)status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK && specialized); xr_xir_compile_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, &diagnostic) == XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(specialized);
    if (output) {
        XrXirCSource emitted = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "language_reference", 1048576, &emitted) == XR_XIR_OK);
        CHECK(emitted.text && emitted.length && emitted.length < 1048576);
        FILE *file = fopen(output, "wb");
        CHECK(file && fwrite(emitted.text, 1, emitted.length, file) == emitted.length && !fclose(file));
        xr_xir_compile_c_source_free(&emitted);
    }
    uint32_t entry = answer_entry(lowered); XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    execute_answer(program, entry, expected, suspensions);
}

int main(int argc, char **argv) {
    CHECK(((argc==4 || argc==5) && !strcmp(argv[1],"--accept")) ||
          (argc==5 && (!strcmp(argv[1],"--reject") || !strcmp(argv[1],"--reject-structure") || !strcmp(argv[1],"--reject-value"))) ||
          ((argc==5 || argc==6) && !strcmp(argv[1],"--emit")));
    unsigned suspensions=0;
    const char *suspend_arg=!strcmp(argv[1],"--accept") && argc==5 ? argv[4] :
        !strcmp(argv[1],"--emit") && argc==6 ? argv[5] : NULL;
    if (suspend_arg) {
        char *tail=NULL;errno=0;unsigned long count=strtoul(suspend_arg,&tail,10);
        CHECK(*suspend_arg>='0' && *suspend_arg<='9' && tail!=suspend_arg && !*tail && !errno && count<=UINT_MAX);
        suspensions=(unsigned)count;
    }
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/%s.xr", XR_LANGUAGE_FIXTURES, argv[2]);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_LANGUAGE_FIXTURES};
    const XrXirSourceRequest request = {session, path, &authority, &owner.context, XR_LANGUAGE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) {
        XrCompileResourceStats failure = {0};
        CHECK(xr_compile_resources_stats(owner.context.resources, &failure) == XR_COMPILE_RESOURCE_OK);
        fprintf(stderr, "source failure ledger: work=%llu allocated=%llu peak=%llu live=%llu\n",
            (unsigned long long)failure.work, (unsigned long long)failure.allocated_bytes,
            (unsigned long long)failure.peak_bytes, (unsigned long long)failure.live_bytes);
    }
    printf("%s source=%u location=%u:%d:%d message=%s\n", argv[2], (unsigned)status,
        diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    char *end = NULL; int64_t expected = (int64_t)strtoll(argv[3], &end, 10);
    CHECK(end && end != argv[3] && !*end);
    if (!strcmp(argv[1], "--reject") || !strcmp(argv[1], "--reject-structure") || !strcmp(argv[1],"--reject-value")) {
        XrXirStatus expected_status=!strcmp(argv[1],"--reject") ? XR_XIR_BAD_TYPE :
            !strcmp(argv[1],"--reject-value") ? XR_XIR_BAD_VALUE : XR_XIR_BAD_STRUCTURE;
        CHECK(status==expected_status && diagnostic.status==status);
        CHECK(diagnostic.line == expected && strstr(diagnostic.message, argv[4]));
        CHECK(!result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    } else {
        CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
        if (!strcmp(argv[2], "init_module_authority")) initialization_authority(&owner, result.checked);
        accept_packet(&owner, &result, session, expected, !strcmp(argv[1], "--emit") ? argv[4] : NULL,suspensions);
    }
    source_fixture_owner_free(&owner);
    printf("%s independent expectation passed; compiler ledger restored\n", argv[2]); return 0;
}
