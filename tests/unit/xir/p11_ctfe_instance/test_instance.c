/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_instance.c - Real closed substitutions survive their Source producers
 *
 * KEY CONCEPT:
 *   Each result owns the verified definition and freshly evaluated value.
 *   Failure and retry retain one original finite compiler owner.
 */
#define XR_IMPORT_MAIN import_original_main
#include "../p11_import/test_import.c"
#include "xir/xxir_ctfe_instance_internal.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_source_instance_internal.h"
#include "instance_foreign.h"

static const char *instance_source_text(void) {
    static const char text[]=
        "fn array<T>()->Array<T>{return []}\nfn optional<T>()->T?{return null}\n"
        "fn tuple<T>()->(T?,Array<T>){return (null,[])}\n"
        "struct Box<T>{private value:T?;static empty()->Box<T>{return Box<T>{value:null}}}\n"
        "fn boxed<T>()->Box<T>{return Box<T>.empty()}\n"
        "fn text<T>()->string{return \"a\\0b\"}\n"
        "fn nested<T>()->Array<T>{return []}\nfn deeper<T>()->i64{return len(nested<Array<Array<T>>>())+42}\n"
        "fn scalar<T>()->i64{return 42}\nfn unit<T>(){}\nfn identity<T>(value:T)->T{return value}\n"
        "export fn run()->i64{const a=array<i8>();const b=array<string>();const c=optional<i8>();const d=optional<string>();"
        "const e=tuple<i8>();const f=tuple<string>();const g=Box<i8>.empty();const h=Box<string>.empty();"
        "const j=boxed<i8>();const k=boxed<string>();"
        "const m=text<i8>();const n=text<string>();"
        "const p=deeper<i8>();const q=deeper<string>();"
        "unit<i8>();unit<string>();return scalar<i8>()+scalar<string>()-identity<i64>(42)}\n";
    return text;
}
static XrXirStatus instance_source_operation(XrXirCompileContext *context,XrXirArtifact **output,const char *kind) {
    const char *text=instance_source_text();
    XrCompilerSession *session=NULL;XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILE_RESOURCE_OK) return p11_resource_status(created);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"instance-constant",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=!strcmp(kind,"foreign")?instance_foreign_source(&request,&source,&diagnostic,NULL,NULL,0):
        xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"Instance Source=%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);XrXirCheckedPacket packet={0};
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *received=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL);
    if (packet.bytes) memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(received,output,NULL);
    xr_xir_compile_artifact_free(received);return status;
}
static XrXirArtifact *instance_source(XrXirCompileContext *context,const char *kind) {
    XrXirArtifact *output=NULL;CHECK(instance_source_operation(context,&output,kind)==XR_XIR_OK && output);return output;
}
static uint32_t instance_named(const XrXirArtifact *artifact,const char *prefix,unsigned ordinal) {
    const XrXirModule *module=&artifact->module;size_t length=strlen(prefix);
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if (body->name_length>length && !memcmp(body->name,prefix,length) && body->name[length]=='$') {
            if (!ordinal) return f;--ordinal;
        }
    }
    CHECK(false);return 0;
}
static XirCtfeInstanceRequest instance_request(const XrXirArtifact *artifact,uint32_t function) {
    return (XirCtfeInstanceRequest){artifact,function,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        {XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},XR_XIR_CTFE_MAX_CALL_DEPTH};
}
static bool instance_argument_absent(const XrXirArtifact *closed,XrXirType argument) {
    const XrXirTypes *source=closed->module.provenance->source->module.types;
    for (uint32_t t=0;source && t<source->count;++t) {
        if (source->nodes[t].parameter_span) continue;
        XrXirStatus status=xr_xir_compile_type_substitution_matches_between(&closed->context,source,closed->module.types,
            NULL,0,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t),argument);
        if (status==XR_XIR_OK) return false;
        CHECK(status==XR_XIR_BAD_TYPE || status==XR_XIR_BAD_STRUCTURE);
    }
    return true;
}
static void instance_normal(const char *kind,unsigned ordinal) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,kind);
    uint32_t f=instance_named(closed,kind,ordinal);XirCtfeInstanceRequest request=instance_request(closed,f);
    XirCtfeInstanceResult *owner=NULL;CHECK(xr_xir_compile_ctfe_instance(&request,NULL,&owner)==XR_XIR_CTFE_OK && owner);
    XirCtfeInstanceView view={0};CHECK(xr_xir_compile_ctfe_instance_view(owner,&view)==XR_XIR_CTFE_OK);
    CHECK(view.checked!=closed && view.definition!=closed->module.provenance->source && view.instance_function==f);
    CHECK(view.checked->context.resources==context.resources && view.lowered->context.resources==context.resources);
    CHECK(view.definition_function==view.checked->module.provenance->origins[f].function);
    CHECK(view.definition->module.generics[view.definition_function].parameter_count==1);
    if (!strcmp(kind,"foreign")) instance_foreign_edges(view.checked);
    XrXirType argument=view.checked->module.provenance->origins[f].arguments[0];
    if (!strcmp(kind,"nested")) {
        CHECK(xr_xir_type_is_array(view.checked->module.types,xr_xir_array_element(view.checked->module.types,argument)));
        CHECK(instance_argument_absent(view.checked,argument));
    }
    else CHECK(argument==XR_XIR_I8 || argument==XR_XIR_STRING);
    if (!strcmp(kind,"scalar") || !strcmp(kind,"foreign")) CHECK(view.value->type==XR_XIR_I64 && view.value->payload==42);
    if (!strcmp(kind,"unit")) CHECK(!view.value->type && !view.value->reserved && !view.value->payload);
    XrXirValue retained={0};CHECK(xr_xir_value_copy(view.value,&retained)==XR_XIR_VALUE_OK);
    xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    view=(XirCtfeInstanceView){0};CHECK(xr_xir_compile_ctfe_instance_view(owner,&view)==XR_XIR_CTFE_OK);
    CHECK(view.definition && view.value && xr_xir_compile_artifact_verify(view.checked,NULL)==XR_XIR_OK);
    xr_xir_compile_ctfe_instance_free(owner);CHECK(xr_xir_value_valid(&retained));
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&retained);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"instance %s[%u] argument=%u original definition/source dead last-owner physical0\n",kind,ordinal,argument);
}
static void instance_limits(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *closed=instance_source(&context,"scalar");
    uint32_t f=instance_named(closed,"scalar",0);XirCtfeInstanceRequest original=instance_request(closed,f);
    XirCtfeInstanceResult *invalid=NULL;XirCtfeInstanceRequest malformed=original;
    CHECK(xr_xir_compile_ctfe_instance(NULL,NULL,&invalid)==XR_XIR_CTFE_BAD_ARGUMENT && !invalid);
    CHECK(xr_xir_compile_ctfe_instance(&original,NULL,NULL)==XR_XIR_CTFE_BAD_ARGUMENT);
    malformed.limits.steps=XR_XIR_CTFE_MAX_STEPS+1;
    CHECK(xr_xir_compile_ctfe_instance(&malformed,NULL,&invalid)==XR_XIR_CTFE_BAD_ARGUMENT && !invalid);
    malformed=original;malformed.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES+1;
    CHECK(xr_xir_compile_ctfe_instance(&malformed,NULL,&invalid)==XR_XIR_CTFE_BAD_ARGUMENT && !invalid);
    malformed=original;malformed.limits.output_bytes=XR_XIR_CTFE_MAX_OUTPUT_BYTES+1;
    CHECK(xr_xir_compile_ctfe_instance(&malformed,NULL,&invalid)==XR_XIR_CTFE_BAD_ARGUMENT && !invalid);
    malformed=original;malformed.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH+1;
    CHECK(xr_xir_compile_ctfe_instance(&malformed,NULL,&invalid)==XR_XIR_CTFE_BAD_ARGUMENT && !invalid);
    malformed=original;malformed.target.abi_version=XR_XIR_VALUE_ABI_VERSION+1;
    CHECK(xr_xir_compile_ctfe_instance(&malformed,NULL,&invalid)==XR_XIR_CTFE_PROFILE && !invalid);
    for (unsigned axis=0;axis<4;++axis) {
        XirCtfeInstanceRequest request=original;
        if (!axis) request.limits.steps=0;else if (axis==1) request.limits.frame_bytes=0;
        else if (axis==2) request.limits.output_bytes=0;else request.call_depth=0;
        XrXirCtfeStatus expected=!axis?XR_XIR_CTFE_STEP_LIMIT:axis==1?XR_XIR_CTFE_FRAME_LIMIT:
            axis==2?XR_XIR_CTFE_OUTPUT_LIMIT:XR_XIR_CTFE_DEPTH_LIMIT;
        XirCtfeInstanceResult *result=NULL;XrCompileResourceStats before=stats(&context);
        CHECK(xr_xir_compile_ctfe_instance(&request,NULL,&result)==expected && !result);
        CHECK(stats(&context).held_bytes==before.held_bytes);
        CHECK(xr_xir_compile_ctfe_instance(&request,NULL,&result)==expected && !result);
        CHECK(xr_xir_compile_ctfe_instance(&original,NULL,&result)==XR_XIR_CTFE_OK);
        xr_xir_compile_ctfe_instance_free(result);CHECK(stats(&context).held_bytes==before.held_bytes);
    }
    XirCtfeInstanceResult *result=NULL;XirCtfeInstanceRequest unsupported=instance_request(closed,instance_named(closed,"identity",0));
    CHECK(xr_xir_compile_ctfe_instance(&unsupported,NULL,&result)==XR_XIR_CTFE_NOT_SUPPORTED && !result);
    unsupported=instance_request(closed,named(&closed->module,"run"));
    CHECK(xr_xir_compile_ctfe_instance(&unsupported,NULL,&result)==XR_XIR_CTFE_NOT_SUPPORTED && !result);
    XirCtfeInstanceResult *held=NULL;CHECK(xr_xir_compile_ctfe_instance(&original,NULL,&held)==XR_XIR_CTFE_OK);
    size_t attempts=source_fixture_compile_attempts;xr_xir_compile_ctfe_instance_free(held);
    CHECK(source_fixture_compile_attempts==attempts);
    xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
#include "instance_resources.h"
#include "source_receiving.h"
#include "source_resources.h"
#include "source_limits.h"
#include "instance_batch.h"
#include "instance_batch_resources.h"
#include "comptime_instances.h"
#include "comptime_instance_resources.h"
#include "binding_cases.h"
#include "binding_resources.h"
#include "binding_wire.h"
#include "literal_cases.h"
#include "literal_controls.h"
#include "literal_resources.h"
#include "literal_receiving_resources.h"
#include "program_cases.h"
#include "program_resources.h"
#include "program_controls.h"
#include "pending_cases.h"
#include "pending_controls.h"
#include "pending_resources.h"
#include "evaluation_cases.h"
#include "evaluation_controls.h"
#include "resolution_cases.h"
#include "resolution_controls.h"
#include "resolution_resources.h"
#include "library_cases.h"
#include "library_resources.h"
#include "library_controls.h"
#include "library_emit.h"
int main(int argc,char **argv) {
    CHECK(argc>=2 && argc<=4);
    if (!strcmp(argv[1],"library-emit")) { CHECK(argc==4);library_comptime_emit(argv[2],argv[3]);return 0; }
    if (!strcmp(argv[1],"library-site-controls")) { CHECK(argc==2);library_comptime_site_controls();return 0; }
    if (!strcmp(argv[1],"library-controls")) { CHECK(argc==2);library_comptime_controls();return 0; }
    if (!strcmp(argv[1],"library-prefix")) { CHECK(argc==3);library_comptime_resources(argv[2],false);return 0; }
    if (!strcmp(argv[1],"library-faults")) { CHECK(argc==3);library_comptime_resources(argv[2],true);return 0; }
    if (!strcmp(argv[1],"library")) { CHECK(argc==3);library_comptime_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"resolution")) { CHECK(argc==3);resolution_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"resolution-controls")) { CHECK(argc==2);resolution_controls();return 0; }
    if (!strcmp(argv[1],"resolution-prefix")) { CHECK(argc==3);resolution_resources(argv[2],false);return 0; }
    if (!strcmp(argv[1],"resolution-faults")) { CHECK(argc==3);resolution_resources(argv[2],true);return 0; }
    if (!strcmp(argv[1],"evaluation")) { CHECK(argc==3);evaluation_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"evaluation-controls")) { CHECK(argc==2);evaluation_controls();return 0; }
    if (!strcmp(argv[1],"pending")) { CHECK(argc==3);pending_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"pending-controls")) { CHECK(argc==2);pending_controls();return 0; }
    if (!strcmp(argv[1],"pending-prefix")) { CHECK(argc==3);pending_resources(argv[2],false);return 0; }
    if (!strcmp(argv[1],"pending-faults")) { CHECK(argc==3);pending_resources(argv[2],true);return 0; }
    if (!strcmp(argv[1],"program")) { CHECK(argc==3);program_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"program-controls")) { CHECK(argc==2);program_controls();return 0; }
    if (!strcmp(argv[1],"program-binding-controls")) { CHECK(argc==2);program_binding_controls();program_batch_controls();return 0; }
    if (!strcmp(argv[1],"program-prefix")) { CHECK(argc==3);program_resources(argv[2],false);return 0; }
    if (!strcmp(argv[1],"program-faults")) { CHECK(argc==3);program_resources(argv[2],true);return 0; }
    if (!strcmp(argv[1],"literal")) { CHECK(argc==3);literal_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"literal-controls")) { CHECK(argc==2);literal_controls();return 0; }
    if (!strcmp(argv[1],"literal-prefix")) { CHECK(argc==3);literal_resources(argv[2],false);return 0; }
    if (!strcmp(argv[1],"literal-faults")) { CHECK(argc==3);literal_resources(argv[2],true);return 0; }
    if (!strcmp(argv[1],"literal-receiving-faults")) { CHECK(argc==2);literal_receiving_resources();return 0; }
    if (!strcmp(argv[1],"limits")) { CHECK(argc==2);instance_limits();return 0; }
    if (!strcmp(argv[1],"tamper")) { CHECK(argc==2);instance_tamper();return 0; }
    if (!strcmp(argv[1],"source-limits")) { CHECK(argc==2);source_receiving_limits();return 0; }
    if (!strcmp(argv[1],"batch-limits")) { CHECK(argc==2);instance_batch_limits();return 0; }
    if (!strcmp(argv[1],"batch")) { CHECK(argc==3);instance_batch_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"batch-prefix")) { CHECK(argc==3);instance_batch_faults(argv[2],true);return 0; }
    if (!strcmp(argv[1],"batch-faults")) { CHECK(argc==3);instance_batch_faults(argv[2],false);return 0; }
    if (!strcmp(argv[1],"batch-bounds")) { CHECK(argc==3);instance_batch_bounds(argv[2]);return 0; }
    if (!strcmp(argv[1],"comptime")) { CHECK(argc==3);comptime_instances_normal(argv[2]);return 0; }
    if (!strcmp(argv[1],"comptime-controls")) { CHECK(argc==2);comptime_instances_controls();return 0; }
    if (!strcmp(argv[1],"comptime-prefix")) { CHECK(argc==3);comptime_instances_faults(argv[2],true);return 0; }
    if (!strcmp(argv[1],"comptime-faults")) { CHECK(argc==3);comptime_instances_faults(argv[2],false);return 0; }
    if (!strcmp(argv[1],"comptime-bounds")) { CHECK(argc==3);comptime_instances_bounds(argv[2]);return 0; }
    if (!strcmp(argv[1],"binding")) { CHECK(argc==2);binding_normal();return 0; }
    if (!strcmp(argv[1],"binding-controls")) { CHECK(argc==2);binding_controls();return 0; }
    if (!strcmp(argv[1],"binding-wire")) { CHECK(argc==2);binding_wire();return 0; }
    if (!strcmp(argv[1],"binding-tamper")) { CHECK(argc==2);binding_tamper();return 0; }
    if (!strcmp(argv[1],"binding-prefix")) { CHECK(argc==2);binding_resources(false);return 0; }
    if (!strcmp(argv[1],"binding-faults")) { CHECK(argc==2);binding_resources(true);return 0; }
    CHECK(argc>=3);unsigned ordinal=(unsigned)strtoul(argv[2],NULL,10);CHECK(ordinal<2);
    CHECK(!strcmp(argv[1],"array") || !strcmp(argv[1],"optional") || !strcmp(argv[1],"tuple") ||
        !strcmp(argv[1],"empty") || !strcmp(argv[1],"boxed") || !strcmp(argv[1],"text") || !strcmp(argv[1],"nested") ||
        !strcmp(argv[1],"scalar") || !strcmp(argv[1],"unit") || !strcmp(argv[1],"foreign"));
    if (argc==4) {
        CHECK(!strcmp(argv[3],"--prefix") || !strcmp(argv[3],"--faults") || !strcmp(argv[3],"--bounds") || !strcmp(argv[3],"--source") ||
            !strcmp(argv[3],"--source-prefix") || !strcmp(argv[3],"--source-faults") || !strcmp(argv[3],"--source-bounds"));
        if (!strcmp(argv[3],"--source")) instance_source_receiving(argv[1],ordinal);
        else if (!strcmp(argv[3],"--source-prefix")) source_receiving_faults(argv[1],ordinal,true);
        else if (!strcmp(argv[3],"--source-faults")) source_receiving_faults(argv[1],ordinal,false);
        else if (!strcmp(argv[3],"--source-bounds")) source_receiving_bounds(argv[1],ordinal);
        else if (!strcmp(argv[3],"--bounds")) instance_bounds(argv[1],ordinal);
        else instance_faults(argv[1],ordinal,!strcmp(argv[3],"--prefix"));
    } else instance_normal(argv[1],ordinal);
    return 0;
}
