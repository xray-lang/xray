/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_iterator_source.c - Governed Iterator annotations through owned Source
 *
 * KEY CONCEPT:
 *   Names and element constraints bind real declarations before executable recipes.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "shared/xnative_declaration.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_compile_owner.h"

static const char iterator_source[] =
    "fn identity(it: Iterator<i64>) -> Iterator<i64> { return it }\n"
    "fn unit(it: Iterator<()>) -> Iterator<()> { return it }\n"
    "fn generic<T>(it: Iterator<T>) -> Iterator<T> { return it }\n"
    "fn use(it: Iterator<i64>) -> Iterator<i64> { return generic(it) }\n"
    "fn name(it: Iterator<string>) -> string { return typeName(it) }\n"
    "fn main() {}\n";
static void iterator_query(const XrXirSourceResult *source) {
    const XrXirSourceView *v=xr_xir_compile_source_snapshot_view(source->snapshot);
    CHECK(v && v->complete && v->types);
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ITERATOR);
    CHECK(native && xr_native_declaration_validate(native));
    uint32_t declaration=0,refs=0;
    for (uint32_t i=0;i<v->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&v->declarations[i];
        if (d->native_identity!=XR_NATIVE_DECLARATION_ITERATOR) continue;
        CHECK(!declaration && d->kind==XR_XIR_SOURCE_TYPE && !strcmp(d->name,"Iterator"));
        CHECK(d->generic_parameter_count==1 && d->generic_constraints);
        CHECK(!d->generic_constraints[0].markers && !d->generic_constraints[0].interfaces &&
            !d->generic_constraints[0].interface_count);
        CHECK(d->range.module<v->module_count);
        const XrXirSourceQueryModule *m=&v->modules[d->range.module];
        CHECK(!strcmp(m->identity,native->identity) && !strcmp(m->path,native->source_path));
        CHECK(!memcmp(&m->fingerprint,&native->source_fingerprint,sizeof(m->fingerprint)));declaration=d->id;
    }
    CHECK(declaration);
    for (uint32_t i=0;i<v->reference_count;++i) if (v->references[i].target==declaration) {
        CHECK(v->references[i].access==XR_XIR_SOURCE_TYPE_USE);++refs;
    }
    CHECK(refs==9);
}
static XrCompileResourceLimits iterator_source_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
}
static XrXirStatus iterator_source_pipeline(const XrXirCompileContext *c,const char *text,bool inspect) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus made=xr_compile_session_new(c->resources,&session);
    XrXirStatus s=made==XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        made==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"iterator-types",NULL};
    XrXirSourceRequest request={session,NULL,&authority,c,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    if (s==XR_XIR_OK) s=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,strlen(text)},&source,&diagnostic,NULL);
    xr_compile_session_free(session);
    if (s!=XR_XIR_OK && diagnostic.status!=XR_XIR_OK && !source_fixture_compile_injected)
        fprintf(stderr,"Source status%u module%u line%d column%d: %s\n",s,diagnostic.module,
            diagnostic.line,diagnostic.column,diagnostic.message);
    if (s==XR_XIR_OK && inspect) {
        CHECK(source.checked && source.snapshot);iterator_query(&source);
        s=xr_xir_compile_checked_write(source.checked,&packet,NULL);
        if (s==XR_XIR_OK) s=xr_xir_compile_checked_read(c,packet.bytes,packet.length,&decoded,NULL);
        xr_xir_compile_source_result_free(&source);
        if (s==XR_XIR_OK) s=xr_xir_compile_artifact_verify(decoded,NULL);
        if (s==XR_XIR_OK) {
            const XrXirModule *m=xr_xir_compile_artifact_module(decoded);uint32_t count=0;
            for (uint32_t t=0;t<m->types->count;++t) if (m->types->nodes[t].kind==XR_XIR_TYPE_ITERATOR) ++count;
            CHECK(count==4);s=xr_xir_compile_specialize(decoded,&closed,NULL);
        }
        if (s==XR_XIR_OK) s=xr_xir_compile_lower(closed,
            &(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL);
        if (s==XR_XIR_OK) s=xr_xir_compile_artifact_verify(lowered,NULL);
    }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&source);return s;
}
static XrXirStatus iterator_source_run(const char *text,bool inspect,XrCompileResourceLimits limits,
    XrCompileResourceStats *observed,bool retry_oom) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    XrXirCompileContext c={0};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&c.resources);
    if (made!=XR_COMPILE_RESOURCE_OK) return made==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    c.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(c.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus s=iterator_source_pipeline(&c,text,inspect);
    CHECK(xr_compile_resources_stats(c.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);if (observed) *observed=after;
    if (retry_oom && s==XR_XIR_OUT_OF_MEMORY) {
        CHECK(source_fixture_compile_injected);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(iterator_source_pipeline(&c,text,inspect)==XR_XIR_OK);
        XrCompileResourceStats retried={0};
        CHECK(xr_compile_resources_stats(c.resources,&retried)==XR_COMPILE_RESOURCE_OK);
        CHECK(retried.live_bytes==baseline.live_bytes && retried.work>after.work &&
            retried.allocated_bytes>after.allocated_bytes);
    }
    xr_compile_resources_release(c.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return s;
}
static void iterator_source_axes(void) {
    XrCompileResourceStats needed={0};
    CHECK(iterator_source_run(iterator_source,true,iterator_source_caps(),&needed,false)==XR_XIR_OK);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t exact=0;exact<2;++exact) {
        XrCompileResourceLimits limits=iterator_source_caps();
        if (!axis) limits.allocated_bytes=needed.allocated_bytes-(exact ? 0 : 1);
        else if (axis==1) limits.live_bytes=needed.peak_bytes-(exact ? 0 : 1);
        else limits.work=needed.work-(exact ? 0 : 1);
        CHECK(iterator_source_run(iterator_source,true,limits,NULL,false)==(exact ? XR_XIR_OK : XR_XIR_BUDGET));
    }
    printf("Iterator Source exact/minus1 axes allocated=%llu peak=%llu work=%llu physical0\n",
        (unsigned long long)needed.allocated_bytes,(unsigned long long)needed.peak_bytes,
        (unsigned long long)needed.work);
}
static void iterator_source_faults(void) {
    size_t sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=ordinal ? ordinal-1 : SIZE_MAX;
        source_fixture_compile_injected=false;
        XrXirStatus s=iterator_source_run(iterator_source,true,iterator_source_caps(),NULL,true);
        if (!ordinal) {CHECK(s==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else {
            if (s!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Source fault ordinal%zu/%zu status%u\n",ordinal,sites,s);
            CHECK(source_fixture_compile_injected && s==XR_XIR_OUT_OF_MEMORY);
        }
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    printf("Iterator complete Source allocation prefixes=%zu physical0\n",sites);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--faults")) {iterator_source_faults();return 0;}
    CHECK(argc==1);iterator_source_axes();
    const struct {const char *text;XrXirStatus expected;} bad[]={
        {"fn bad(it: Iterator<i64,string>) {}\nfn main() {}",XR_XIR_BAD_TYPE},
        {"fn bad<Iterator>(it: Iterator<i64>) {}\nfn main() {}",XR_XIR_BAD_STRUCTURE},
        {"const Iterator=1\nfn bad(it: Iterator<i64>) {}\nfn main() {}",XR_XIR_BAD_TYPE},
        {"fn bad(it: Iterator<i64>) -> i64 { return it.next(1) }\nfn main() {}",XR_XIR_BAD_TYPE},
        {"fn bad(it: Task<Iterator<i64>>) {}\nfn main() {}",XR_XIR_BAD_TYPE}};
    for (uint32_t i=0;i<sizeof(bad)/sizeof(*bad);++i)
        CHECK(iterator_source_run(bad[i].text,false,iterator_source_caps(),NULL,false)==bad[i].expected);
    puts("Governed Source Iterator annotations, owned packet, exact query and authority refusals PASS");return 0;
}
