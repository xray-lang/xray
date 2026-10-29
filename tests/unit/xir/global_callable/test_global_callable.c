/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_global_callable.c - Owned global callback inputs and sealed producer refusal
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include "../global_callable_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(global_callable_source)-1==704,"complete original Source size");
_Static_assert(sizeof(global_callable_expected)-1==13,"complete original output size");
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats.held_bytes;
}
static void *copy_bytes(XrCompileResources *owner,const void *input,size_t bytes) {
    void *output=NULL;CHECK(bytes && xr_compile_resources_alloc(owner,bytes,&output)==XR_COMPILE_RESOURCE_OK);
    memcpy(output,input,bytes);return output;
}
static void reject_changed_producers(XrXirArtifact *artifact) {
    const XrXirModule *module=&artifact->module;const XrXirProvenance *provenance=module->provenance;
    CHECK(provenance && provenance->contracts && provenance->contract_count==module->function_count);
    uint32_t caller=UINT32_MAX,record=UINT32_MAX,reference=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];const XrXirFunctionEffectContract *contract=&provenance->contracts[f];
        for(uint32_t v=0;v<contract->value_count;++v) {
            const XrXirRootValueIdentity *identity=&contract->values[v];
            if(identity->mode!=XR_XIR_EFFECT_VALUE_CALL_BIND || identity->instruction>=body->instruction_count)continue;
            const XrXirInstruction *op=&body->instructions[identity->instruction];
            if(op->op!=XR_XIR_COPY || op->args[0]<body->parameter_count)continue;
            uint32_t input=op->args[0]-body->parameter_count;
            if(input>=body->instruction_count || body->instructions[input].op!=XR_XIR_SLOT_LOAD)continue;
            for(uint32_t i=0;i<input;++i) {
                if(body->instructions[i].op==XR_XIR_FUNCTION_REF && body->instructions[i].type==op->type) {
                    caller=f;record=v;reference=body->parameter_count+i;
                }
            }
        }
    }
    CHECK(caller!=UINT32_MAX && record!=UINT32_MAX && reference!=UINT32_MAX);
    XrCompileResources *owner=artifact->context.resources;uint64_t before=held(owner);
    const XrXirFunction *body=&module->functions[caller];const XrXirFunctionEffectContract *contract=&provenance->contracts[caller];
    XrXirFunction *functions=copy_bytes(owner,module->functions,module->function_count*sizeof(*functions));
    XrXirInstruction *instructions=copy_bytes(owner,body->instructions,body->instruction_count*sizeof(*instructions));
    uint32_t ordinal=contract->values[record].instruction;
    CHECK(instructions[ordinal].args[0]!=reference);instructions[ordinal].args[0]=reference;functions[caller].instructions=instructions;
    XrXirArtifact invalid=*artifact;invalid.module.functions=functions;
    CHECK(xr_xir_compile_artifact_verify(&invalid,NULL)!=XR_XIR_OK);
    xr_compile_resources_free(instructions);xr_compile_resources_free(functions);
    XrXirFunctionEffectContract *contracts=copy_bytes(owner,provenance->contracts,provenance->contract_count*sizeof(*contracts));
    XrXirRootValueIdentity *values=copy_bytes(owner,contract->values,contract->value_count*sizeof(*values));
    values[record].declared_type=XR_XIR_I64;contracts[caller].values=values;
    XrXirProvenance changed=*provenance;changed.contracts=contracts;invalid=*artifact;invalid.module.provenance=&changed;
    CHECK(xr_xir_compile_artifact_verify(&invalid,NULL)!=XR_XIR_OK);
    xr_compile_resources_free(values);xr_compile_resources_free(contracts);
    CHECK(held(owner)==before && xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
}
int main(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t before=held(context.resources);
    char source[sizeof(global_callable_source)];memcpy(source,global_callable_source,sizeof(source));
    const uint8_t expected_digest[]={0xdc,0xe3,0xc1,0xc7,0x34,0x44,0x93,0x9b,0xce,0x47,0x1d,0x91,0xc0,0xe9,0x53,0xf6,
        0x3e,0x0b,0x1e,0x6f,0xac,0x27,0x3f,0x99,0x22,0xef,0xf8,0x40,0x7d,0xfd,0xec,0x23};
    uint8_t digest[32];xr_sha256((const uint8_t *)source,sizeof(source)-1,digest);
    CHECK(!memcmp(digest,expected_digest,sizeof(digest)));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"global-callable",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,source,sizeof(source)-1};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked);memset(source,'!',sizeof(source));xr_compile_session_free(session);
    XrXirArtifact *artifact=result.checked;result.checked=NULL;xr_xir_compile_source_result_free(&result);
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);reject_changed_producers(artifact);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);artifact=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&artifact,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
    reject_changed_producers(artifact);xr_xir_compile_artifact_free(artifact);
    CHECK(held(context.resources)==before);xr_compile_resources_release(context.resources);
    puts("Global callable Source: original704 bytes, retired inputs, exact packet intake and producer/advertisement refusals PASS");return 0;
}
