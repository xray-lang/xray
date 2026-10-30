/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Real Source nominal bridges observe the original request failure and cleanup. */
#include "hir/xhir_nominal.h"
#include "xir/xxir_type_arena.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "../../xir/xir_source_compile_owner.h"
typedef struct NominalControlProbe {
    const XrXirTypes *arena;
    uint64_t now;
    unsigned stage,mode,arena_calls,receiving_calls;
    bool fired,cancelled,clock_failure;
    XrXirStatus observed;
} NominalControlProbe;
static NominalControlProbe nominal_probe;
static uint64_t nominal_control_clock(void) {
    return nominal_probe.clock_failure?0:nominal_probe.now?nominal_probe.now:xr_time_monotonic_ns();
}
/* Only the clock provider changes; initialization, deadline precedence and
 * failure latching are the real request implementation used by Source. */
#define xr_time_monotonic_ns nominal_control_clock
#include "base/xcompile_request.c"
#undef xr_time_monotonic_ns
static bool nominal_control_cancel(const void *unused) {(void)unused;return nominal_probe.cancelled;}
static const XrXirTypes *nominal_control_arena(const XrXirTypeArena *arena) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);nominal_probe.arena=types;return types;
}
static XrXirStatus nominal_control_view(const char *file,int line,const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirNominalView *out,const char **reason) {
    bool bridge=strstr(file,"xxir_source_comptime_nominal.inc.c")!=NULL;
    unsigned stage=types==nominal_probe.arena?1u:2u;
    if(bridge) {
        if(stage==1)++nominal_probe.arena_calls;else ++nominal_probe.receiving_calls;
        if(nominal_probe.mode&&nominal_probe.stage==stage&&!nominal_probe.fired) {
            CHECK(control&&control->initialized);nominal_probe.fired=true;
            if(nominal_probe.mode==1)nominal_probe.cancelled=true;
            else if(nominal_probe.mode==2){CHECK(control->has_deadline);nominal_probe.now=control->deadline_ns;}
            else nominal_probe.clock_failure=true;
            fprintf(stderr,"actual Nominal Source bridge %s:%d stage%u mode%u\n",file,line,stage,nominal_probe.mode);
        }
    }
    XrXirStatus status=xr_hir_nominal_view(types,type,control,out,reason);
    if(bridge&&nominal_probe.fired)nominal_probe.observed=status;
    return status;
}
/* Function-like interception preserves the actual call-site provenance and
 * forwards every decision to the sole production structural receiver. */
#define xr_xir_compile_type_arena_types nominal_control_arena
#define xr_hir_nominal_view(...) nominal_control_view(__FILE__,__LINE__,__VA_ARGS__)
#include "xir/xxir_source.c"
#undef xr_hir_nominal_view
#undef xr_xir_compile_type_arena_types
static void nominal_source_trial(const char *text,size_t length,unsigned stage,unsigned mode) {
    nominal_probe=(NominalControlProbe){.stage=stage,.mode=mode};
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompileRequestOptions options={.cancel_poll=nominal_control_cancel};
    if(mode>=2){options.deadline_mode=XR_COMPILE_DEADLINE_AFTER;options.timeout_ns=UINT64_C(10000000000);}
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"nominal-control",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    xr_compile_session_free(session);
    XrXirStatus expected=!mode?XR_XIR_OK:mode==1?XR_XIR_CANCELLED:mode==2?XR_XIR_DEADLINE:XR_XIR_CLOCK_FAILURE;
    fprintf(stderr,"Nominal Source stage%u mode%u status%u diagnostic%u observed%u arena%u receive%u\n",
        stage,mode,(unsigned)status,(unsigned)diagnostic.status,(unsigned)nominal_probe.observed,
        nominal_probe.arena_calls,nominal_probe.receiving_calls);
    if(mode) {
        CHECK(nominal_probe.fired&&nominal_probe.observed==expected);
        CHECK(status==expected&&diagnostic.status==expected&&!result.checked&&!result.snapshot);
    } else {
        CHECK(status==XR_XIR_OK&&diagnostic.status==XR_XIR_OK&&result.checked&&result.snapshot);
        CHECK(nominal_probe.arena_calls&&nominal_probe.receiving_calls);
        CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
    }
    xr_xir_compile_source_result_free(&result);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    if(mode) {
        XrCompileResourceStatus latched=control.status;nominal_probe=(NominalControlProbe){0};
        XrCompileRequestOptions fresh_options={0};XrCompileRequestControl fresh={0};
        CHECK(xr_compile_request_control_init(&fresh_options,&fresh)==XR_COMPILE_RESOURCE_OK);
        session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_bind_request(session,&fresh)==XR_COMPILE_RESOURCE_OK);
        request.session=session;diagnostic=(XrXirSourceDiagnostic){0};
        CHECK(xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL)==XR_XIR_OK);
        xr_compile_session_free(session);CHECK(result.checked&&result.snapshot);
        CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&result);
        CHECK(xr_compile_request_control_poll(&control)==latched);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    }
    nominal_probe=(NominalControlProbe){0};xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live&&!source_fixture_compile_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==2);FILE *file=fopen(argv[1],"rb");CHECK(file&&fseek(file,0,SEEK_END)==0);long end=ftell(file);
    CHECK(end>0&&fseek(file,0,SEEK_SET)==0);size_t length=(size_t)end;char *text=malloc(length);
    CHECK(text&&fread(text,1,length,file)==length&&fclose(file)==0);
    nominal_source_trial(text,length,0,0);
    for(unsigned stage=1;stage<=2;++stage)for(unsigned mode=1;mode<=3;++mode) {
        nominal_source_trial(text,length,stage,mode);
    }
    free(text);puts("Actual Source Nominal arena/materialization interruption, cleanup and fresh requests PASS");return 0;
}
