/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_emit.h - Complete authentic Library results become ordinary C11
 */
#include "xir/xxir_emit_c.h"
static void library_comptime_emit(const char *kind,const char *path) {
    XrXirCompileContext context;context_new(&context);
    XrXirArtifact *source=NULL,*received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(library_comptime_operation(&context,kind,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirCSource emitted={0};CHECK(xr_xir_compile_emit_c(lowered,"p11_library_native",1048576,&emitted)==XR_XIR_OK);
    CHECK(emitted.text && emitted.length);FILE *file=fopen(path,"wb");
    CHECK(file && fwrite(emitted.text,1,emitted.length,file)==emitted.length);
    uint32_t count=lowered->module.function_count;
    /* Test observers carry the real emitted ordinal in each callback, including
     * cleanup callbacks where ordinary current-entry observation is forbidden. */
    CHECK(fprintf(file,"\nextern XrXirAction p11_library_observe_resume(XrXirCallView *,uint32_t);\nextern void p11_library_observe_release(XrXirCallView *,XrXirCallStatus,uint32_t);\nconst uint32_t p11_library_callback_count=%uu;\n",count)>0);
    for(uint32_t f=0;f<count;++f) CHECK(fprintf(file,
        "static XrXirAction p11_observer_resume_%u(XrXirCallView *v){return p11_library_observe_resume(v,%uu);}\n"
        "static void p11_observer_release_%u(XrXirCallView *v,XrXirCallStatus r){p11_library_observe_release(v,r,%uu);}\n",f,f,f,f)>0);
    CHECK(fprintf(file,"const XrXirResumeEntry p11_library_resume_callbacks[%u]={",count)>0);
    for(uint32_t f=0;f<count;++f) CHECK(fprintf(file,"%sp11_observer_resume_%u",f?",":"",f)>0);
    CHECK(fprintf(file,"};\nconst XrXirReleaseEntry p11_library_release_callbacks[%u]={",count)>0);
    for(uint32_t f=0;f<count;++f) CHECK(fprintf(file,"%sp11_observer_release_%u",f?",":"",f)>0);
    CHECK(fprintf(file,"};\n")>0 && !fclose(file));
    fprintf(stderr,"Library %s authentic emitted C11 bytes%zu with complete ordinary proof; execution pending\n",kind,emitted.length);
    xr_xir_compile_c_source_free(&emitted);xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
