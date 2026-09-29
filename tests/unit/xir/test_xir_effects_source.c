/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects_source.c - Control facts after source destruction and specialization
 */
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_witness_cases.h"
static void source_generic_effects(const XrXirModule *module, const XrXirEffects *effects) {
    bool open=false; uint32_t found=0;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->generics && module->generics[f].parameter_count) open=true;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        bool use=fn->name_length==10 && !memcmp(fn->name,"genericUse",10);
        bool missing=fn->name_length==10 && !memcmp(fn->name,"missingUse",10);
        bool parameter=fn->name_length>=10 && !memcmp(fn->name,"throwParam",10);
        if (!use && !missing && !parameter) continue;
        ++found;
        CHECK(xr_xir_effects_function(effects,f)->throws==XR_XIR_EFFECT_MAY);
        CHECK(!xr_xir_effects_error_unknown(effects,f));
        CHECK(!xr_xir_effects_error_unidentified(effects,f));
        if (parameter) CHECK(xr_xir_effects_error(effects,f,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
            XR_XIR_ERROR_SYMBOLIC_VARIANT)==open);
        if (use) {
            uint32_t matches=0;
            for (uint32_t t=0;t<module->types->count;++t) {
                XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
                if (!xr_xir_effects_error(effects,f,type,0)) continue;
                const XrXirTypeNode *node=xr_xir_type_node(module->types,type);
                CHECK(node && node->nominal.argument_count==1 && node->nominal.arguments[0]==XR_XIR_I64);
                ++matches;
            }
            CHECK(matches==1);
        }
    }
    CHECK(found==3);
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        if (fn->name_length==19 && !memcmp(fn->name,"closureHandledCycle",19))
            CHECK(xr_xir_effects_function(effects,f)->throws==XR_XIR_EFFECT_NONE);
    }

    const char *names[]={"closureCaught","closureDeepCaught","closureWrong","closureUnion","closureCallbackCaught"};
    for (uint32_t n=0;n<5;++n) {
        bool seen=false;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];
            if (fn->name_length!=strlen(names[n]) || memcmp(fn->name,names[n],fn->name_length)) continue;
            seen=true;
            CHECK(xr_xir_effects_function(effects,f)->throws==(n==2 ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
            CHECK(!xr_xir_effects_error_unknown(effects,f) && !xr_xir_effects_error_unidentified(effects,f));
        }
        CHECK(seen);
    }

}
static void source_effects(const XrXirArtifact *artifact) {
    const struct { const char *name; XrXirEffect suspend, throws; } expected[] = {
        {"typedHandled", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"variantHandled", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"variantUnmatched", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"typedRethrow", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"dynamicRethrow", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN},
        {"dynamicTyped", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN},
        {"wrongTyped", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"parameterThrow", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"unknownParameter", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"localOverwrite", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"phiThrow", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"loopThrow", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"recursiveError", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"recursivePeer", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellAfterCall", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellSnapshot", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellAfterCleanup", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellCleanupSnapshot", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellPanicCleanup", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellPanicSnapshot", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellErrorCleanup", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellErrorSnapshot", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"panicThrows", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"cellAfterYield", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_MAY},
        {"cellAfterOutput", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"genericCaught", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"genericHandledUse", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"paramForward", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"paramCaught", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"dynamicKnownMix", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_MAY},
        {"pure", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"sleeper", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE},
        {"relay", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE},
        {"instantiated", XR_XIR_EFFECT_MAY, XR_XIR_EFFECT_NONE},
        {"fail", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"forward", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"handled", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE},
        {"rethrow", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_MAY},
        {"dynamic", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_UNKNOWN},
        {"dynamicHandled", XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_NONE},
        {"panicOnly", XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_NONE}};
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_effects_analyze(artifact, NULL, &effects) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    for (size_t e = 0; e < sizeof(expected) / sizeof(expected[0]); ++e) {
        bool found = false;
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            size_t length = strlen(expected[e].name);
            if (function->name_length < length || memcmp(function->name, expected[e].name, length) ||
                (function->name_length != length && function->name[length] != '$')) continue;
            const XrXirFunctionEffects *fact = xr_xir_effects_function(effects, f);
            CHECK(fact);
            if (fact->suspend != expected[e].suspend || fact->throws != expected[e].throws)
                fprintf(stderr, "effect %s: suspend=%u throws=%u\n", expected[e].name, fact->suspend, fact->throws);
            CHECK(fact->suspend == expected[e].suspend && fact->throws == expected[e].throws); found = true;
            uint32_t mask = expected[e].throws == XR_XIR_EFFECT_MAY ? 1u : 0u;
            if (!strcmp(expected[e].name,"unknownParameter")) mask=0;
            if (!strcmp(expected[e].name,"cellAfterCleanup") || !strcmp(expected[e].name,"cellPanicCleanup") ||
                !strcmp(expected[e].name,"cellErrorCleanup")) mask=3;
            if (!strcmp(expected[e].name,"parameterThrow") || !strcmp(expected[e].name,"phiThrow") ||
                !strcmp(expected[e].name,"loopThrow") || !strcmp(expected[e].name,"cellAfterCall") ||
                !strcmp(expected[e].name,"cellAfterYield") || !strcmp(expected[e].name,"cellAfterOutput") || !strcmp(expected[e].name,"paramForward")) mask = 3;
            if (!strcmp(expected[e].name,"localOverwrite") || !strcmp(expected[e].name,"recursiveError") ||
                !strcmp(expected[e].name,"recursivePeer") || !strcmp(expected[e].name,"panicThrows")) mask = 2;
            for (uint32_t t = 0; t < module->types->count; ++t) {
                XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
                if (!xr_xir_type_is_enum(module->types,type)) continue;
                uint32_t d = module->types->nodes[t].nominal.declaration;
                const XrXirNominalTable *table = module->types->nominals;
                XrXirLiteral name = table->declarations ? table->declarations[d].name : table->identities[d].name;
                bool fault = name.length == 5 && !memcmp(name.bytes,"Fault",5);
                if (xr_xir_effects_error(effects,f,type,0) != (fault && (mask&1) != 0))
                    fprintf(stderr,"error atom %s type=%u name=%.*s fault=%u mask=%u bad=%u stage=%u\n",expected[e].name,type,(int)name.length,name.bytes,fault,mask,xr_xir_effects_error(effects,f,type,0),module->stage);
                CHECK(xr_xir_effects_error(effects,f,type,0) == (fault && (mask&1) != 0));
                if (fault) CHECK(xr_xir_effects_error(effects,f,type,1) == ((mask&2) != 0));
            }
            CHECK(xr_xir_effects_error_unidentified(effects,f)==(!strcmp(expected[e].name,"unknownParameter")));
            CHECK(xr_xir_effects_error_unknown(effects,f) == (expected[e].throws == XR_XIR_EFFECT_UNKNOWN || !strcmp(expected[e].name,"dynamicKnownMix")));
        }
        if (!found) fprintf(stderr, "missing effect function %s at stage %u\n", expected[e].name, module->stage);
        CHECK(found);
    }
    source_generic_effects(module,effects);
    effect_witness_paths(module,effects);
    xr_xir_effects_free(effects);
}
int main(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_EFFECT_FIXTURES};
    XrXirSourceRequest request = {session, XR_EFFECT_FIXTURES "/root.xr", &authority, NULL, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "%u at %d:%d: %s\n", status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK); source_effects(result.checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet); source_effects(checked);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); source_effects(closed);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); source_effects(lowered); xr_xir_artifact_free(lowered);
    puts("Source control effects survived packets, specialization and lowering");
    return 0;
}
