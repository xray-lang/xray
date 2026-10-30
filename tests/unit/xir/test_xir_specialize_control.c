/*
 * xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License.
 * Synchronous controlled receiving of complete Source and owned packets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir/xxir.h"
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "xir_library_compile_owner.h"
#include "base/xcompile_request.h"
#include "os/os_time.h"
static bool specialize_clock_controlled;
static uint64_t specialize_clock_now;
static uint64_t specialize_observe_clock(void);
#define xr_time_monotonic_ns specialize_observe_clock
#include "base/xcompile_request.c"
#undef xr_time_monotonic_ns
#include "specialize_control_probe.h"
#include "xir/xxir.c"
#include "xir/xxir_checked.c"
#include "base/xhash.h"
static uint32_t specialize_probe_hash(uint32_t,const void *,size_t);
#define xr_hash_bytes_update specialize_probe_hash
#include "xir/xxir_specialize.c"
#undef xr_hash_bytes_update
#include "xir/xxir_ctfe_instance.c"
#include "xir/xxir_generic.c"
#undef xir_lower_sha256_span
#undef xir_receive_compare
#undef xr_compile_request_control_poll
#undef xir_type_copy_zero
#undef xir_lower_copy_span
#undef xir_compile_alloc
#undef xir_receive_step
#undef xir_receive_poll
#include "implementation_control_fixture.h"

SpecializeControlProbe *specialize_control_probe;
static const char *specialize_seen[256];
static size_t specialize_seen_count;
static uint32_t specialize_probe_hash(uint32_t hash,const void *bytes,size_t size) {
    uint32_t result=xr_hash_bytes_update(hash,bytes,size);SpecializeControlProbe *p=specialize_control_probe;
    if(p&&p->control){CHECK(size<=4096);p->hash_bytes+=size;p->hash_value=result;}
    return result;
}
static bool specialize_witness(SpecializeControlProbe *p);
static uint64_t specialize_observe_clock(void) {
    SpecializeControlProbe *p=specialize_control_probe;
    if(specialize_clock_controlled&&p&&p->mode!=0&&p->mode!=3&&specialize_witness(p))
        specialize_clock_now=p->mode==1?200:0;
    return specialize_clock_controlled?specialize_clock_now:xr_time_monotonic_ns();
}
static void specialize_seen_caller(const char *caller) {
    if(!caller)return;
    for(size_t i=0;i<specialize_seen_count;++i)if(!strcmp(specialize_seen[i],caller))return;
    CHECK(specialize_seen_count<256);specialize_seen[specialize_seen_count++]=caller;
}
static const char *specialize_enter(XrCompileRequestControl *control,const char *caller) {
    SpecializeControlProbe *p=specialize_control_probe;
    if(!p||p->control!=control)return NULL;
    const char *prior=p->caller;p->caller=caller;return prior;
}
static void specialize_leave(XrCompileRequestControl *control,const char *prior) {
    SpecializeControlProbe *p=specialize_control_probe;if(p&&p->control==control)p->caller=prior;
}
XrCompileResourceStatus specialize_probe_poll(XrCompileRequestControl *control,const char *caller) {
    const char *prior=specialize_enter(control,caller);
    XrCompileResourceStatus status=xr_compile_request_control_poll(control);specialize_leave(control,prior);return status;
}
XrXirStatus specialize_probe_receive_poll(XrCompileRequestControl *control,const char *caller) {
    return control?xir_compile_resource_status(specialize_probe_poll(control,caller)):XR_XIR_OK;
}
XrXirStatus specialize_probe_step(XrCompileRequestControl *control,uint32_t *rows,const char *caller) {
    const char *prior=specialize_enter(control,caller);XrXirStatus status=xir_receive_step(control,rows);
    specialize_leave(control,prior);return status;
}
void *specialize_probe_alloc(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status,const char *caller) {
    (void)caller;void *memory=xir_compile_alloc(context,bytes,status);
    SpecializeControlProbe *p=specialize_control_probe;
    if(memory&&p&&p->zero_active&&p->zero_bytes==bytes&&!p->zero_destination){
        p->zero_destination=memory;memset(memory,0xa5,bytes);
    }
    return memory;
}
void *specialize_probe_memset(void *destination,int value,size_t bytes,const char *caller) {
    void *result=memset(destination,value,bytes);SpecializeControlProbe *p=specialize_control_probe;
    if(p&&p->zero_active&&p->zero_destination&&value==0&&!strcmp(caller,"xir_type_copy_zero")) {
        CHECK(destination==p->zero_destination+p->zero_written&&bytes<=4096);
        p->zero_written+=bytes;CHECK(p->zero_written<=p->zero_bytes);
    }
    return result;
}
XrCompileResourceStatus specialize_probe_span(XrCompileRequestControl *control,void *destination,
    const void *source,size_t bytes,const char *caller) {
    SpecializeControlProbe *p=specialize_control_probe;
    if(!p||p->control!=control||bytes<=4096)return xir_lower_copy_span(control,destination,source,bytes);
    const char *prior=specialize_enter(control,caller);
    CHECK(!p->copy_destination);p->copy_destination=destination;p->copy_source=source;p->copy_bytes=bytes;
    memset(destination,0xa5,bytes);
    XrCompileResourceStatus status=xir_lower_copy_span(control,destination,source,bytes);
    if(status==XR_COMPILE_RESOURCE_OK)CHECK(!memcmp(destination,source,bytes));
    p->copy_destination=NULL;p->copy_source=NULL;p->copy_bytes=0;specialize_leave(control,prior);return status;
}
void *specialize_probe_zero(const XrXirCompileContext *context,size_t count,size_t size,XrXirStatus *status,
    XrCompileRequestControl *control,const char *caller) {
    SpecializeControlProbe *p=specialize_control_probe;
    if(!p||p->control!=control||!size||count>SIZE_MAX/size||count*size<=4096)
        return xir_type_copy_zero(context,count,size,status,control);
    const char *prior=specialize_enter(control,caller);
    CHECK(!p->zero_active);p->zero_active=true;p->zero_bytes=count*size;p->zero_written=0;p->zero_destination=NULL;
    void *result=xir_type_copy_zero(context,count,size,status,control);
    p->zero_active=false;p->zero_destination=NULL;p->zero_bytes=0;p->zero_written=0;
    specialize_leave(control,prior);return result;
}
int specialize_probe_memcmp(const void *left,const void *right,size_t bytes,const char *caller) {
    int result=memcmp(left,right,bytes);SpecializeControlProbe *p=specialize_control_probe;
    if(p&&p->compare_left&&!strcmp(caller,"xir_receive_compare")) {
        CHECK(left==p->compare_left+p->compare_done&&right==p->compare_right+p->compare_done&&bytes<=4096);
        p->compare_done+=bytes;CHECK(p->compare_done<=p->compare_bytes);
    }
    return result;
}
XrXirStatus specialize_probe_compare(XrCompileRequestControl *control,const void *left,const void *right,
    size_t bytes,int *output,const char *caller) {
    SpecializeControlProbe *p=specialize_control_probe;
    if(!p||p->control!=control||bytes<=4096)return xir_receive_compare(control,left,right,bytes,output);
    const char *prior=specialize_enter(control,caller);CHECK(!p->compare_left);
    p->compare_left=left;p->compare_right=right;p->compare_bytes=bytes;p->compare_done=0;
    XrXirStatus status=xir_receive_compare(control,left,right,bytes,output);
    p->compare_left=NULL;p->compare_right=NULL;p->compare_bytes=0;p->compare_done=0;
    specialize_leave(control,prior);return status;
}
XrCompileResourceStatus specialize_probe_sha(XrCompileRequestControl *control,XrSHA256Context *hash,
    const void *source,size_t bytes,const char *caller) {
    SpecializeControlProbe *p=specialize_control_probe;
    if(!p||p->control!=control||bytes<=4096)return xir_lower_sha256_span(control,hash,source,bytes);
    const char *prior=specialize_enter(control,caller);CHECK(!p->sha);p->sha=hash;p->sha_start=*hash;p->sha_source=source;p->sha_bytes=bytes;
    XrCompileResourceStatus status=xir_lower_sha256_span(control,hash,source,bytes);
    p->sha=NULL;p->sha_source=NULL;p->sha_bytes=0;specialize_leave(control,prior);return status;
}
static bool specialize_witness(SpecializeControlProbe *p) {
    if(!p->wanted||p->fired)return false;
    bool hit=p->caller&&!strcmp(p->wanted,p->caller);
    if(!strcmp(p->wanted,"copy-prefix")&&p->copy_destination&&p->copy_bytes>4096&&
        !memcmp(p->copy_destination,p->copy_source,4096)) {
        for(size_t i=4096;i<p->copy_bytes;++i)CHECK(p->copy_destination[i]==0xa5);
        hit=true;
    }
    if(!strcmp(p->wanted,"zero-prefix")&&p->zero_destination&&p->zero_written==4096&&p->zero_bytes>4096) {
        for(size_t i=0;i<4096;++i)CHECK(p->zero_destination[i]==0);
        for(size_t i=4096;i<p->zero_bytes;++i)CHECK(p->zero_destination[i]==0xa5);
        hit=true;
    }
    if(!strcmp(p->wanted,"hash-prefix")&&p->hash_bytes==4096) {CHECK(p->hash_value==UINT32_C(1523400133));hit=true;}
    if(!strcmp(p->wanted,"compare-prefix")&&p->compare_done==4096&&p->compare_bytes>4096)hit=true;
    if(!strcmp(p->wanted,"sha-prefix")&&p->sha&&p->sha->count-p->sha_start.count==4096&&p->sha_bytes>4096) {
        XrSHA256Context copy=*p->sha,reference=p->sha_start;uint8_t observed[32],expected[32];
        xr_sha256_final(&copy,observed);xr_sha256_update(&reference,p->sha_source,4096);xr_sha256_final(&reference,expected);
        CHECK(!memcmp(observed,expected,32));hit=true;
    }
    if(!hit||++p->phase_hits<p->cut||p->mode==3)return false;
    p->fired=true;++p->witnesses;
    return true;
}
static bool specialize_cancel(const void *opaque) {
    SpecializeControlProbe *p=(SpecializeControlProbe *)opaque;CHECK(p==specialize_control_probe);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(p->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    ++p->polls;specialize_seen_caller(p->caller);
    if(p->artifact_slot)CHECK(!*p->artifact_slot);
    return (p->mode==0||p->mode==3)&&specialize_witness(p);
}
static void specialize_begin(SpecializeControlProbe *p,XrCompileRequestControl *control,
    XrCompileResources *resources,const char *wanted,unsigned mode) {
    *p=(SpecializeControlProbe){.resources=resources,.control=control,.wanted=wanted,.mode=mode,.cut=1};
    *control=(XrCompileRequestControl){0};XrCompileRequestOptions options={.cancel_poll=specialize_cancel,.cancel_context=p};
    if(mode==1||mode==2){specialize_clock_controlled=true;specialize_clock_now=100;options.deadline_mode=XR_COMPILE_DEADLINE_AFTER;options.timeout_ns=100;}
    CHECK(xr_compile_request_control_init(&options,control)==XR_COMPILE_RESOURCE_OK);specialize_control_probe=p;
}
static void specialize_end(SpecializeControlProbe *p,XrXirStatus status) {
    CHECK(p==specialize_control_probe);
    if(p->wanted&&p->mode!=3){XrXirStatus expected=p->mode==0?XR_XIR_CANCELLED:p->mode==1?XR_XIR_DEADLINE:XR_XIR_CLOCK_FAILURE;
        if(status!=expected)fprintf(stderr,"phase %s mode%u actual%u expected%u witnesses%zu\n",p->wanted,p->mode,status,expected,p->witnesses);
        CHECK(status==expected&&p->witnesses==1&&p->fired);
        XrCompileResourceStatus sticky=p->control->status;CHECK(xr_compile_request_control_poll(p->control)==sticky);
    }else CHECK(status==XR_XIR_OK);
    specialize_control_probe=NULL;specialize_clock_controlled=false;
}
static XrXirStatus specialize_source(const XrXirCompileContext *context,const char *text,size_t bytes,
    XrCompileRequestControl *control,XrXirArtifact **output) {
    XrCompilerSession *session=NULL;XrCompileResourceStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILE_RESOURCE_OK)return library_compile_resource_status(opened);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"specialize-control-complete",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,bytes};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    xr_compile_session_free(session);
    if(status==XR_XIR_OK){*output=result.checked;result.checked=NULL;}
    else if(status!=XR_XIR_CANCELLED&&status!=XR_XIR_DEADLINE&&status!=XR_XIR_CLOCK_FAILURE)
        fprintf(stderr,"Source status%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_xir_compile_source_result_free(&result);return status;
}
static void specialize_shape_controls(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirNominalDeclaration declarations[70]={0};XrXirConstructionRow rows[70]={0};uint32_t kinds[70]={0};
    XrXirNominalTable table={.declarations=declarations,.count=70};XrXirTypes types={.nominals=&table};
    XrXirConstruction construction={.context=owner.context,.rows=rows,.count=70,.kinds=kinds};
    XrXirFunctionEffectContract contracts[70]={0};XrXirProvenance proof={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=70};
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirConstruction *copy=NULL;XrXirStatus status;
        specialize_begin(&probe,&control,owner.context.resources,"construction_rows_shape",mode);
        status=xir_construction_clone(&owner.context,&types,&construction,&copy,&control);
        specialize_end(&probe,status);CHECK(!copy);
        specialize_begin(&probe,&control,owner.context.resources,"provenance_storage_shape",mode);
        XrXirProvenance *cloned=provenance_clone(&owner.context,&proof,&status,&control);
        specialize_end(&probe,status);CHECK(!cloned);
    }
    char name[12300];memset(name,'x',sizeof(name));
    static const unsigned char three[]={0xe2,0x82,0xac},four[]={0xf0,0x9f,0x98,0x80};
    memcpy(name+4095,three,sizeof(three));memcpy(name+8190,four,sizeof(four));
    CHECK(xr_utf8_core_scan_strict((const uint8_t *)name,sizeof(name)).error==XR_UTF8_OK);
    CHECK(construction_name_utf8(name,sizeof(name),NULL)==XR_XIR_OK);
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;
        specialize_begin(&probe,&control,owner.context.resources,"construction_utf8_read",mode);
        XrXirStatus status=construction_name_utf8(name,sizeof(name),&control);specialize_end(&probe,status);
    }
    XrXirSlotFact slot={0,name,sizeof(name),0};XrXirSlotFacts slots={&slot,1};
    XrXirConstruction borrowed={.context=owner.context,.slots=slots};
    uint64_t held=library_compile_stats(&owner.context).held_bytes;
    XrXirConstruction *peer=NULL;CHECK(xir_construction_clone(&owner.context,NULL,&borrowed,&peer,NULL)==XR_XIR_OK);
    uint64_t with_peer=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirConstruction *copy=NULL;
        specialize_begin(&probe,&control,owner.context.resources,"copy-prefix",mode);
        XrXirStatus status=xir_construction_clone(&owner.context,NULL,&borrowed,&copy,&control);
        specialize_end(&probe,status);CHECK(!copy&&library_compile_stats(&owner.context).held_bytes==with_peer);
        bool same=true;specialize_begin(&probe,&control,owner.context.resources,"compare-prefix",mode);
        status=xir_construction_compare(&owner.context,NULL,&borrowed,peer,&same,&control);
        specialize_end(&probe,status);CHECK(same&&library_compile_stats(&owner.context).held_bytes==with_peer);
    }
    xr_xir_compile_construction_free(peer);CHECK(library_compile_stats(&owner.context).held_bytes==held);
    name[4096]='!';CHECK(xr_utf8_core_scan_strict((const uint8_t *)name,sizeof(name)).error!=XR_UTF8_OK);
    CHECK(construction_name_utf8(name,sizeof(name),NULL)==XR_XIR_BAD_STRUCTURE);
    memcpy(name+4096,three+1,1);memset(name+8191,0x80,1);
    CHECK(construction_name_utf8(name,sizeof(name),NULL)==XR_XIR_BAD_STRUCTURE);
    library_compile_owner_drop(&owner);puts("shape rows/provenance and strict UTF8 crossing 4096 boundaries cancel/deadline/clock rollback PASS");
}
static void specialize_copy_controls(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    char text[13000];size_t used=0;
    const char *begin="export fn run()->string{return \"";used=strlen(begin);memcpy(text,begin,used);
    memset(text+used,'x',12300);used+=12300;memcpy(text+used,"\"}\n",3);used+=3;
    XrXirArtifact *source=NULL;CHECK(specialize_source(&owner.context,text,used,NULL,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);memset(text,'!',used);
    uint64_t held=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirArtifact *received=NULL;
        specialize_begin(&probe,&control,owner.context.resources,"copy-prefix",mode);
        probe.artifact_slot=(const XrXirArtifact *const *)&received;
        XrXirStatus status=xir_checked_read(&owner.context,packet.bytes,packet.length,&received,NULL,&control);
        specialize_end(&probe,status);CHECK(!received&&library_compile_stats(&owner.context).held_bytes==held);
    }
    XrXirArtifact *received=NULL;CHECK(xir_checked_read(&owner.context,packet.bytes,packet.length,&received,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(received);xr_xir_compile_checked_packet_free(&packet);library_compile_owner_drop(&owner);
    puts("additional complete12300B String literal decoder actual4096 copy prefix/tail controls PASS");
}
static void specialize_hash_controls(void) {
    unsigned char bytes[8193];for(size_t i=0;i<sizeof(bytes);++i)bytes[i]=(unsigned char)(i*37+11);
    static const struct {size_t bytes;uint32_t hash;} vectors[]={
        {0,UINT32_C(2166136261)},{1,UINT32_C(235694922)},{4095,UINT32_C(3954752161)},
        {4096,UINT32_C(1523400133)},{4097,UINT32_C(3206040394)},{8192,UINT32_C(2625494469)},{8193,UINT32_C(673459018)}};
    for(size_t i=0;i<sizeof(vectors)/sizeof(*vectors);++i)CHECK(xr_hash_bytes(bytes,vectors[i].bytes)==vectors[i].hash);
    static const unsigned char zero[]={204,36,49,196};CHECK(xr_hash_bytes(zero,sizeof(zero))==0);
    CHECK(xr_hash_bytes(NULL,0)==XR_FNV_OFFSET_BASIS&&xr_hash_bytes_update(17,NULL,0)==17);
    uint32_t hash=xr_hash_bytes_update(XR_FNV_OFFSET_BASIS,bytes,4095);
    hash=xr_hash_bytes_update(hash,bytes+4095,1);hash=xr_hash_bytes_update(hash,bytes+4096,sizeof(bytes)-4096);
    CHECK(hash==UINT32_C(673459018));
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    for(unsigned mode=0;mode<3;++mode) {
        SpecializeControlProbe probe;XrCompileRequestControl control;
        specialize_begin(&probe,&control,owner.context.resources,"hash-prefix",mode);
        SpecContext context={.remaining=owner.context,.control=&control};uint32_t output=19;
        CHECK(!spec_hash(&context,bytes,sizeof(bytes),&output));specialize_end(&probe,context.diagnostic.status);CHECK(output==19);
    }
    SpecContext context={.remaining=owner.context};uint32_t output=19;
    CHECK(spec_hash(&context,zero,sizeof(zero),&output)&&output==0);
    library_compile_owner_drop(&owner);puts("frozen FNV1a zero/empty and 4095/4096/4097/8193 split boundaries + actual4096 prefix controls PASS");
}
static void specialize_complete_controls(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    char text[8000];size_t bytes=implementation_control_fixture(text,sizeof(text));XrXirArtifact *source=NULL;
    CHECK(specialize_source(&owner.context,text,bytes,NULL,&source)==XR_XIR_OK);
    XrXirCheckedPacket original={0};CHECK(xr_xir_compile_checked_write(source,&original,NULL)==XR_XIR_OK);
    static const char *const phases[]={"zero-prefix","sha-prefix","checked_read_mode","checked_array_storage","source_facts_match"};
    uint64_t held=library_compile_stats(&owner.context).held_bytes;
    for(unsigned mode=0;mode<3;++mode)for(size_t phase=0;phase<sizeof(phases)/sizeof(*phases);++phase) {
        SpecializeControlProbe probe;XrCompileRequestControl control;XrXirArtifact *received=NULL;
        specialize_begin(&probe,&control,owner.context.resources,phases[phase],mode);probe.artifact_slot=(const XrXirArtifact *const *)&received;
        XrXirStatus status=xir_checked_read(&owner.context,original.bytes,original.length,&received,NULL,&control);
        specialize_end(&probe,status);CHECK(!received&&library_compile_stats(&owner.context).held_bytes==held);
    }
    SpecializeControlProbe probe;XrCompileRequestControl control;XrXirArtifact *received=NULL,*closed=NULL;
    specialize_begin(&probe,&control,owner.context.resources,NULL,3);
    CHECK(xir_checked_read(&owner.context,original.bytes,original.length,&received,NULL,&control)==XR_XIR_OK);
    XrXirCheckedPacket same={0};CHECK(xir_checked_write(received,&same,NULL,&control)==XR_XIR_OK);
    CHECK(same.length==original.length&&!memcmp(same.bytes,original.bytes,same.length));
    xr_xir_compile_checked_packet_free(&same);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL,&control)==XR_XIR_OK);specialize_end(&probe,XR_XIR_OK);
    xr_xir_compile_artifact_free(received);xr_xir_compile_artifact_free(source);memset(text,'!',bytes);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_checked_packet_free(&original);library_compile_owner_drop(&owner);
    puts("complete7658 normal packet identity and decoder prefix controls/new request recovery PASS");
}
#include "specialize_control_transactions.inc.c"
int main(void) {
    specialize_shape_controls();specialize_copy_controls();specialize_hash_controls();specialize_complete_controls();
    specialize_source_transactions();specialize_pending_transactions();specialize_take_transactions();specialize_large_evaluation_controls();specialize_generic_controls();
    for(size_t i=0;i<specialize_seen_count;++i)printf("actual controlled body %s\n",specialize_seen[i]);
    library_compile_observer_free();return 0;
}
