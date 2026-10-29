/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "program/xr_xir_source_product.h"
#include "base/xsha256.h"
static bool product_packet_poll(const void *context) {
    ProductPacketProbe *probe=(ProductPacketProbe *)context;++probe->polls;
    if(probe->disabled||!probe->destination||probe->witnesses)return false;
    bool complete=probe->destination[probe->bytes-1]==probe->source[probe->bytes-1];
    if(probe->mode==4||probe->mode==3) {
        if(!complete)return false;
        CHECK(!memcmp(probe->destination,probe->source,probe->bytes));++probe->witnesses;
        return probe->mode==4;
    }
    if(memcmp(probe->destination,probe->source,4096))return false;
    CHECK(!complete);
    for(size_t i=4096;i+1<probe->bytes;++i)CHECK(probe->destination[i]==0xa5);
    ++probe->witnesses;
    if(probe->mode==1) {
        CHECK(probe->control->has_deadline);uint64_t now=xr_time_monotonic_ns();CHECK(now);
        while(now<probe->control->deadline_ns){xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();CHECK(now);}
        return false;
    }
    CHECK(probe->mode==0);return true;
}
static XrXirSourceProductRequest product_packet_request(XrCompileResources *resources,
    XrCompilerSession *session,const XrModuleIdentityAuthority *authority,XrXirCompileContext *context) {
    *context=(XrXirCompileContext){resources,xr_xir_compile_default_limits()};
    XrXirSourceRequest source={session,NULL,authority,context,XR_PUBLIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    return (XrXirSourceProductRequest){source,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
}
static void product_packet_after_program(XrXirSourceProduct *product) {
    XrXirSourceProductPacketView before[2]={{0}};
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(product);CHECK(facts);
    uint8_t digest[2][32];
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_compile_source_product_packet(product,(XrXirSourceProductPacketKind)i,&before[i])==XR_XIR_OK);
        xr_sha256(before[i].bytes,before[i].length,digest[i]);
        CHECK(!memcmp(digest[i],i?facts->closed_digest:facts->source_digest,32));
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK&&program);
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i) {
        XrXirSourceProductPacketView packet={0};XrXirArtifact *received=NULL;
        CHECK(xr_xir_compile_source_product_packet(product,(XrXirSourceProductPacketKind)i,&packet)==XR_XIR_OK);
        CHECK(packet.bytes==before[i].bytes&&packet.length==before[i].length);
        xr_sha256(packet.bytes,packet.length,digest[i]);
        CHECK(!memcmp(digest[i],i?facts->closed_digest:facts->source_digest,32));
        CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),packet.bytes,packet.length,&received,NULL)==XR_XIR_OK&&received);
        xr_xir_compile_artifact_free(received);
    }
}
static void product_packet_equal(char *text,size_t length,const char *name) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);uint64_t held=sdk_stats(resources).held_bytes;
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};XrXirCompileContext context={0};
    XrXirSourceProductRequest request=product_packet_request(resources,session,&authority,&context);
    XrXirSourceText source={NULL,text,length};XrXirSourceResult checked={0};XrXirSourceDiagnostic immediate={0};
    CHECK(xr_xir_compile_source_check_text(&request.source,&source,&checked,&immediate,NULL)==XR_XIR_OK);
    XrXirCheckedPacket expected[2]={{0}};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_checked_write(checked.checked,&expected[0],NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked.checked,&closed,NULL)==XR_XIR_OK&&closed);
    CHECK(xr_xir_compile_checked_write(closed,&expected[1],NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&checked);
    xr_compile_session_free(session);session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);request.source.session=session;
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_product_build_text(&request,&source,&product,&diagnostic)==XR_XIR_OK&&product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);memset(text,'!',length);
    for(unsigned i=0;i<2;++i) {
        XrXirSourceProductPacketView packet={0};
        CHECK(xr_xir_compile_source_product_packet(product,(XrXirSourceProductPacketKind)i,&packet)==XR_XIR_OK);
        CHECK(packet.length==expected[i].length&&!memcmp(packet.bytes,expected[i].bytes,packet.length));
        fprintf(stderr,"product packet %s kind%u same-authority bytes%zu wholeSHA and post-Program strict read\n",name,i,packet.length);
        xr_xir_compile_checked_packet_free(&expected[i]);
    }
    product_packet_after_program(product);xr_xir_compile_source_product_free(product);
    CHECK(sdk_stats(resources).held_bytes==held);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
}
static uint64_t product_packet_copy_case(unsigned mode,uint64_t timeout_ns) {
    char text[8000];size_t length=implementation_control_fixture(text,sizeof(text));CHECK(length==7658);
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);uint64_t held=sdk_stats(resources).held_bytes;
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    ProductPacketProbe probe={.resources=resources,.mode=mode};XrCompileRequestControl control={0};probe.control=&control;
    XrCompileRequestOptions options={.cancel_poll=product_packet_poll,.cancel_context=&probe};
    if(mode==1){options.deadline_mode=XR_COMPILE_DEADLINE_AFTER;options.timeout_ns=timeout_ns;}
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"packet-copy-control",NULL};XrXirCompileContext context={0};
    XrXirSourceProductRequest request=product_packet_request(resources,session,&authority,&context);
    XrXirSourceText input={NULL,text,length};XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    CHECK(!product_packet_probe);product_packet_probe=&probe;uint64_t start=xr_time_monotonic_ns();CHECK(start);
    XrXirStatus status=xr_xir_compile_source_product_build_text(&request,&input,&product,&diagnostic);
    uint64_t elapsed=xr_time_monotonic_ns()-start;product_packet_probe=NULL;
    probe.source=NULL;probe.destination=NULL;probe.control=NULL;
    fprintf(stderr,"product packet copy mode%u status%u stage%u bytes%zu allocations%zu witnesses%zu oom%u elapsedNs%llu\n",mode,(unsigned)status,(unsigned)diagnostic.stage,probe.bytes,probe.allocations,probe.witnesses,probe.oom_seen?1u:0u,(unsigned long long)elapsed);
    CHECK(probe.bytes>4096&&probe.allocations==1);
    if(mode==3) {
        CHECK(status==XR_XIR_OK&&product&&probe.witnesses==1);
        xr_compile_session_free(session);session=NULL;memset(text,'!',length);
        product_packet_after_program(product);xr_xir_compile_source_product_free(product);
    } else {
        XrXirStatus expected=mode==1?XR_XIR_DEADLINE:mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_CANCELLED;
        CHECK(status==expected&&!product&&diagnostic.status==expected&&diagnostic.xir.status==expected&&diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CLOSED_PACKET);
        CHECK(mode==2?probe.oom_seen:probe.witnesses==1);
        if(mode!=2)CHECK(control.status==(mode==1?XR_COMPILE_RESOURCE_DEADLINE:XR_COMPILE_RESOURCE_CANCELLED));
    }
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    CHECK(sdk_stats(resources).held_bytes==held);
    if(mode!=3) {
        /* Finite expiry proves rejection. Recovery uses a new default request;
         * cancellation and physical OOM retain the original request options. */
        probe.disabled=true;control=(XrCompileRequestControl){0};
        if(mode==1){options.deadline_mode=XR_COMPILE_DEADLINE_NONE;options.timeout_ns=0;}
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);request.source.session=session;
        CHECK(xr_xir_compile_source_product_build_text(&request,&input,&product,NULL)==XR_XIR_OK&&product);
        xr_compile_session_free(session);product_packet_after_program(product);xr_xir_compile_source_product_free(product);
        CHECK(sdk_stats(resources).held_bytes==held);
    }
    xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    if(mode!=3) {
        XrSourceCompileOptions recovery=xray_source_compile_options_default();recovery.stdlib_path=XR_PUBLIC_STDLIB;
        XrSourceInput source={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"packet-copy-recovery",text,length}};
        XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};
        CHECK(xray_source_build(&source,&recovery,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
        CHECK(failure.status==XR_SOURCE_OK&&failure.cleanup_status==XR_SOURCE_OK);
        DeclarationsCopyProbe retired={0};implementation_execute(artifact,text,length,&retired);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    return elapsed;
}
static void product_packet_cases(void) {
    char implementation[8000];size_t length=implementation_control_fixture(implementation,sizeof(implementation));
    product_packet_equal(implementation,length,"packet-implementation");
    char flow[4096];FILE *file=fopen(XR_PUBLIC_STDLIB "/../demos/01-basics/control_flow.xr","rb");CHECK(file);
    size_t flow_length=fread(flow,1,sizeof(flow),file);CHECK(!ferror(file)&&feof(file)&&flow_length==2127&&fclose(file)==0);
    product_packet_equal(flow,flow_length,"packet-control-flow");
    uint64_t baseline=product_packet_copy_case(3,0);CHECK(baseline<UINT64_MAX-UINT64_C(5000000000));
    product_packet_copy_case(0,0);product_packet_copy_case(4,0);product_packet_copy_case(2,0);
    product_packet_copy_case(1,baseline+UINT64_C(5000000000));
    puts("Owned Source/Closed packets: exact authority bytes, whole digests, Program retirement, controlled copy and fresh recovery PASS");
}
