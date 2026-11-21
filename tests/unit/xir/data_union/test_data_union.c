/* Finite ordinary union: real Source/HIR/Checked ownership and failure gates. */
#include "xir/xxir_data_union.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_types.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_value.h"
#include "hir/xhir_literal.h"
#include "hir/xhir_conversion.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_source_compile_owner.h"
static const char source[]="var values: Array<string | i64> = [1, \"hello\"]\n";
static void zero(void) {CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);}
static XrXirStatus check_text(const char *text,size_t length,XrCompileRequestControl *control,XrXirSourceResult *output) {
    XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return xir_compile_resource_status(made);
    context.limits=xr_xir_compile_default_limits();XrCompilerSession *session=NULL;
    made=xr_compile_session_new(context.resources,&session);
    if(made==XR_COMPILE_RESOURCE_OK && control)made=xr_compile_session_bind_request(session,control);
    XrXirStatus status=xir_compile_resource_status(made);
    if(status==XR_XIR_OK) {
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"finite-data-union",NULL};
        XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM, .target = XR_XIR_HOST_TARGET};
        XrXirSourceDiagnostic diagnostic={0};
        status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,length},output,&diagnostic,NULL);
        CHECK(diagnostic.status==status);
    }
    xr_compile_session_free(session);xr_compile_resources_release(context.resources);return status;
}
static void facts(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module && module->stage==XR_XIR_CHECKED);
    uint32_t unions=0,injections=0;XrXirType type=XR_XIR_UNIT;
    for(uint32_t t=0;t<module->types->count;++t)if(module->types->nodes[t].kind==XR_XIR_TYPE_DATA_UNION) {
        type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);++unions;
        CHECK(xr_xir_data_union_shape(module->types,type));
        CHECK(xr_xir_data_union_contains(module->types,type,XR_XIR_STRING));
        CHECK(xr_xir_data_union_contains(module->types,type,XR_XIR_I64));
        CHECK(!xr_xir_data_union_contains(module->types,type,XR_XIR_ERROR));
    }
    CHECK(unions==1);
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        for(uint32_t i=0;i<function->instruction_count;++i)if(function->instructions[i].op==XR_XIR_DATA_UNION_INJECT) {
            const XrXirInstruction *in=&function->instructions[i];
            CHECK(in->type==type);CHECK(xr_xir_data_union_contains(module->types,type,xr_xir_operand_type(function,in->args[0])));++injections;
        }
    }
    CHECK(injections==2);XrXirDiagnostic diagnostic={0};CHECK(xr_xir_compile_artifact_verify(artifact,&diagnostic)==XR_XIR_OK);
}
static void packet_failures(const XrXirArtifact *producer) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCheckedPacket measured={0};XrXirDiagnostic diagnostic={0};
    source_fixture_compile_attempts=0;
    CHECK(xr_xir_compile_checked_write(producer,&measured,&diagnostic)==XR_XIR_OK);
    size_t writer_sites=source_fixture_compile_attempts;CHECK(writer_sites);
    xr_xir_compile_checked_packet_free(&measured);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    for(size_t p=0;p<writer_sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p;source_fixture_compile_injected=false;
        XrXirCheckedPacket refused={0};
        CHECK(xr_xir_compile_checked_write(producer,&refused,&diagnostic)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !refused.bytes && !refused.length);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer,&packet,&diagnostic)==XR_XIR_OK);
    live=source_fixture_compile_live;bytes=source_fixture_compile_bytes;
    size_t reader_sites=0;
    for(size_t p=0;p<=reader_sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p?p-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
        XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
        XrXirStatus status=xir_compile_resource_status(made);XrXirArtifact *received=NULL;
        if(status==XR_XIR_OK) {
            context.limits=xr_xir_compile_default_limits();
            status=xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,&diagnostic);
        }
        if(!p) {CHECK(status==XR_XIR_OK);reader_sites=source_fixture_compile_attempts;CHECK(reader_sites);facts(received);}
        else {CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !received);}
        xr_xir_compile_artifact_free(received);if(context.resources)xr_compile_resources_release(context.resources);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    xr_xir_compile_checked_packet_free(&packet);
    printf("Finite data union packet FI: writer=%zu reader=%zu\n",writer_sites,reader_sites);
}
static void owned_receiving(void) {
    char *buffer=malloc(sizeof(source)-1);CHECK(buffer);memcpy(buffer,source,sizeof(source)-1);
    XrXirSourceResult result={0};CHECK(check_text(buffer,sizeof(source)-1,NULL,&result)==XR_XIR_OK);
    memset(buffer,0xa5,sizeof(source)-1);free(buffer);facts(result.checked);packet_failures(result.checked);
    XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,&diagnostic)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirArtifact *received=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,&diagnostic)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);facts(received);
    XrXirTarget target=XR_XIR_HOST_TARGET;XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_lower(received,&target,NULL,&lowered,&diagnostic)!=XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(received);zero();
}
typedef struct Cancel {size_t polls,trigger;} Cancel;
static bool cancel(const void *opaque) {Cancel *state=(Cancel *)opaque;++state->polls;return state->trigger && state->polls>=state->trigger;}
static void failures(void) {
    size_t sites=0;
    for(size_t p=0;p<=sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p?p-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirSourceResult result={0};XrXirStatus status=check_text(source,sizeof(source)-1,NULL,&result);
        if(!p) {CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);facts(result.checked);}
        else {CHECK(source_fixture_compile_injected && status==XR_XIR_OUT_OF_MEMORY);CHECK(!result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);zero();
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    size_t polls=0;
    for(size_t p=0;p<=polls;++p) {
        Cancel state={0,p};XrCompileRequestControl control={0};
        CHECK(xr_compile_request_control_init(&(XrCompileRequestOptions){.cancel_poll=cancel,.cancel_context=&state},&control)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceResult result={0};XrXirStatus status=check_text(source,sizeof(source)-1,&control,&result);
        if(!p) {CHECK(status==XR_XIR_OK);polls=state.polls;CHECK(polls);}
        else {CHECK(status==XR_XIR_CANCELLED);CHECK(!result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);zero();
    }
    printf("Finite data union: FI=%zu cancel-prefixes=%zu physical=0/0\n",sites,polls);
}
static void canonical(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirTypes empty={0};XrXirDataUnion output={0};
    XrXirType members[]={XR_XIR_STRING,XR_XIR_I32,XR_XIR_STRING,XR_XIR_I32};
    CHECK(xr_xir_compile_data_union(&context,&empty,members,4,NULL,&output)==XR_XIR_OK && output.count==2);
    /* Canonical order is type identity order, independent of spelling order. */
    CHECK(output.members[0]==XR_XIR_STRING && output.members[1]==XR_XIR_I32);
    XrXirCallableParameter parameters[2]={{output.members[0],0,0},{output.members[1],0,0}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_DATA_UNION,.parameters=parameters,.parameter_count=2};
    XrXirTypes types={.nodes=&node,.count=1};XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_data_union_shape(&types,type));XrXirType selected=XR_XIR_UNIT;
    /* Auto-wrap requires the exact member: no implicit numeric widening. */
    CHECK(xr_xir_data_union_select(&types,XR_XIR_I32,type,&selected)==XR_XIR_OK && selected==XR_XIR_I32);
    CHECK(xr_xir_data_union_select(&types,XR_XIR_STRING,type,&selected)==XR_XIR_OK && selected==XR_XIR_STRING);
    CHECK(xr_xir_data_union_select(&types,XR_XIR_I16,type,&selected)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_data_union_select(&types,XR_XIR_I64,type,&selected)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_data_union_select(&types,XR_XIR_U8,type,&selected)==XR_XIR_BAD_TYPE);
    XrHirLiteral literal={0};const char *reason=NULL;
    CHECK(xr_hir_literal_check(&types,&(XrHirLiteralInput){.magnitude=7,.expected=true,.expected_type=type},NULL,&literal,&reason)==XR_XIR_OK && literal.type==XR_XIR_I32);
    XrHirConversion conversion={0};XrHirConversionContext hir={.compile=&context,.types=&types};
    CHECK(xr_hir_conversion_check(&hir,&(XrHirConversionInput){.actual=XR_XIR_I32,.has_expected=true,.expected=type},&conversion,&reason)==XR_XIR_OK);
    CHECK(conversion.kind==XR_HIR_CONVERSION_DATA_UNION && conversion.selected_member==XR_XIR_I32);
    CHECK(xr_hir_conversion_instruction(&conversion,3).op==XR_XIR_DATA_UNION_INJECT);
    CHECK(xr_hir_conversion_check(&hir,&(XrHirConversionInput){.actual=XR_XIR_I16,.has_expected=true,.expected=type},&conversion,&reason)==XR_XIR_BAD_TYPE);
    /* Two members of one numeric family are legal: the tag separates them. */
    XrXirType same_family[]={XR_XIR_I16,XR_XIR_I32};
    XrXirDataUnion family={0};
    CHECK(xr_xir_compile_data_union(&context,&empty,same_family,2,NULL,&family)==XR_XIR_OK && family.count==2);
    CHECK(family.members[0]==XR_XIR_I16 && family.members[1]==XR_XIR_I32);
    XrXirCallableParameter family_parameters[2]={{family.members[0],0,0},{family.members[1],0,0}};
    XrXirTypeNode family_node={.kind=XR_XIR_TYPE_DATA_UNION,.parameters=family_parameters,.parameter_count=2};
    XrXirTypes family_types={.nodes=&family_node,.count=1};
    XrHirLiteral ambiguous={0};
    CHECK(xr_hir_literal_check(&family_types,&(XrHirLiteralInput){.magnitude=7,.expected=true,
        .expected_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE},NULL,&ambiguous,&reason)==XR_XIR_BAD_TYPE &&
        reason && strstr(reason,"ambiguous"));
    xr_compile_resources_free(family.members);
    XrXirDataUnion unchanged=output;XrXirType invalid[]={XR_XIR_ERROR,XR_XIR_I32};
    CHECK(xr_xir_compile_data_union(&context,&empty,invalid,2,NULL,&output)==XR_XIR_BAD_TYPE && !memcmp(&output,&unchanged,sizeof(output)));
    invalid[0]=XR_XIR_ERROR;invalid[1]=XR_XIR_I16;
    CHECK(xr_xir_compile_data_union(&context,&empty,invalid,2,NULL,&output)==XR_XIR_BAD_TYPE);
    parameters[0].mode=XR_PARAM_REF;CHECK(!xr_xir_data_union_shape(&types,type));parameters[0].mode=0;
    node.kind=XR_XIR_TYPE_UNION;CHECK(!xr_xir_data_union_shape(&types,type));
    xr_compile_resources_free(unchanged.members);
    xr_compile_resources_release(context.resources);zero();
}
int main(void) {canonical();owned_receiving();failures();puts("Finite data union static Source/HIR/Checked PASS; payload carriers beyond the admitted scalars remain refused");return 0;}
