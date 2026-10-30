/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef SPECIALIZE_CONTROL_PROBE_H
#define SPECIALIZE_CONTROL_PROBE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_lower_control.h"
#include "os/os_time.h"
#ifndef CHECK
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#endif

typedef struct SpecializeControlProbe {
    XrCompileResources *resources;
    XrCompileRequestControl *control;
    const char *caller,*wanted;
    const unsigned char *copy_source;
    unsigned char *copy_destination,*zero_destination;
    size_t copy_bytes,zero_bytes,zero_written,polls,witnesses,phase_hits,cut;
    size_t hash_bytes;uint32_t hash_value;
    const unsigned char *compare_left,*compare_right,*sha_source;
    size_t compare_bytes,compare_done,sha_bytes;
    XrSHA256Context *sha;
    XrSHA256Context sha_start;
    unsigned mode;
    bool fired,zero_active;
    const XrXirArtifact *const *artifact_slot;
} SpecializeControlProbe;
extern SpecializeControlProbe *specialize_control_probe;
XrCompileResourceStatus specialize_probe_poll(XrCompileRequestControl *,const char *);
XrXirStatus specialize_probe_receive_poll(XrCompileRequestControl *,const char *);
XrXirStatus specialize_probe_step(XrCompileRequestControl *,uint32_t *,const char *);
void *specialize_probe_alloc(const XrXirCompileContext *,size_t,XrXirStatus *,const char *);
void *specialize_probe_memset(void *,int,size_t,const char *);
int specialize_probe_memcmp(const void *,const void *,size_t,const char *);
XrXirStatus specialize_probe_compare(XrCompileRequestControl *,const void *,const void *,size_t,int *,const char *);
XrCompileResourceStatus specialize_probe_sha(XrCompileRequestControl *,XrSHA256Context *,const void *,size_t,const char *);
XrCompileResourceStatus specialize_probe_span(XrCompileRequestControl *,void *,const void *,size_t,const char *);
void *specialize_probe_zero(const XrXirCompileContext *,size_t,size_t,XrXirStatus *,XrCompileRequestControl *,const char *);

/* These adapters delegate to the same helpers and touch only unpublished storage. */
#define xir_compile_alloc(c,n,s) specialize_probe_alloc((c),(n),(s),__func__)
#define xir_lower_copy_span(c,d,s,n) specialize_probe_span((c),(d),(s),(n),__func__)
#define memset(d,v,n) specialize_probe_memset((d),(v),(n),__func__)
#define memcmp(a,b,n) specialize_probe_memcmp((a),(b),(n),__func__)
#include "xir/xxir_receive_control.h"
#undef memcmp
#undef memset
#undef xir_lower_copy_span
#undef xir_compile_alloc
#endif

#define xir_receive_poll(c) specialize_probe_receive_poll((c),__func__)
#define xir_receive_step(c,p) specialize_probe_step((c),(p),__func__)
#define xir_compile_alloc(c,n,s) specialize_probe_alloc((c),(n),(s),__func__)
#define xir_lower_copy_span(c,d,s,n) specialize_probe_span((c),(d),(s),(n),__func__)
#define xir_type_copy_zero(c,n,s,r,q) specialize_probe_zero((c),(n),(s),(r),(q),__func__)
#define xir_receive_compare(c,a,b,n,o) specialize_probe_compare((c),(a),(b),(n),(o),__func__)
#define xir_lower_sha256_span(c,h,s,n) specialize_probe_sha((c),(h),(s),(n),__func__)
#define xr_compile_request_control_poll(c) specialize_probe_poll((c),__func__)
