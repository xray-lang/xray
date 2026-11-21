/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Typed origins survive the producer and roll back every receiving allocation. */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
static void map_literal_packet_digest(XrXirCheckedPacket *packet) {
    XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet->bytes,32);
    xr_sha256_update(&sha,packet->bytes+64,packet->length-64);xr_sha256_final(&sha,packet->bytes+32);
}
#include "test_body_map_library.inc.c"
static void map_literal_packet_owners(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"map-origin-test",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&compile,NULL,NULL,XR_XIR_PROGRAM,NULL, XR_XIR_HOST_TARGET};
    char bytes[]="var base = {\"a\": 1}\nvar merged = {...base, \"b\": 2}\n";
    XrXirSourceText text={NULL,bytes,sizeof(bytes)-1};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Map memory Source: %u %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));
    xr_xir_compile_source_snapshot_free(result.snapshot);result.snapshot=NULL;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(result.checked);result.checked=NULL;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t first=physical_attempts,live=physical_live,held=physical_bytes;
    XrXirArtifact *decoded=NULL;CHECK(xr_xir_compile_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    size_t sites=physical_attempts-first;CHECK(sites>0);xr_xir_compile_artifact_free(decoded);decoded=NULL;
    for(unsigned old=0;old<2;++old) {
        size_t offset=old?12:8;uint8_t original[4];memcpy(original,packet.bytes+offset,4);
        uint32_t value=old?129u:42u;
        for(unsigned n=0;n<4;++n)packet.bytes[offset+n]=(uint8_t)(value>>(8*n));
        map_literal_packet_digest(&packet);
        CHECK(xr_xir_compile_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
        memcpy(packet.bytes+offset,original,4);map_literal_packet_digest(&packet);
    }
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;
        status=xr_xir_compile_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL);
        if(status!=XR_XIR_OUT_OF_MEMORY || decoded)fprintf(stderr,"Map packet fault %zu/%zu: status %u output %u\n",point,sites,(unsigned)status,decoded!=NULL);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !decoded);
        physical_fail=SIZE_MAX;
        CHECK(physical_live==live && physical_bytes==held && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    }
    CHECK(xr_xir_compile_checked_read(&compile,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *module=xr_xir_compile_artifact_module(decoded);
    CHECK(module && module->declarations && module->declarations->map_literal_count==2);
    const XrXirMapLiteral *literal=&module->declarations->map_literals[1];
    CHECK(!literal->file.bytes && !literal->file.length && literal->count==2 && literal->items[0].kind==1 && literal->items[0].line==2 && literal->items[0].column==15 &&
        literal->items[1].kind==0 && literal->items[1].line==2 && literal->items[1].column==24);
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    xr_compile_resources_release(resources);CHECK(!physical_live && !physical_bytes);
}
static void map_literal_metadata_owners(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirSourceModule module={.name="m",.name_length=1};XrXirFunctionIdentity functions[2]={{0},{0}};
    char file[]="app.xr";
    XrXirMapLiteralOrigin items[]={{0,3,12},{1,4,9}};
    XrXirMapLiteral literal={.file={file,6},.items=items,.count=2};
    XrXirDeclarations source={.modules=&module,.module_count=1,.functions=functions,.entry_function=1,.map_literals=&literal,.map_literal_count=1};
    CHECK(xr_xir_compile_declarations_verify(&compile,&source,NULL,2,XR_XIR_PROGRAM, NULL)==XR_XIR_OK);
    XrXirDeclarations *copy=NULL;
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    size_t live=physical_live,bytes=physical_bytes,first=physical_attempts;
    CHECK(xr_xir_compile_declarations_clone(&compile,&source,2,NULL,&copy)==XR_XIR_OK);
    size_t sites=physical_attempts-first;
    xr_xir_compile_declarations_free(copy);copy=NULL;CHECK(sites>0);
    for(size_t site=0;site<sites;++site) {
        physical_fail=physical_attempts+site;
        CHECK(xr_xir_compile_declarations_clone(&compile,&source,2,NULL,&copy)==XR_XIR_OUT_OF_MEMORY && !copy);
        physical_fail=SIZE_MAX;
        CHECK(physical_live==live && physical_bytes==bytes && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK &&
            after.held_bytes==baseline.held_bytes);
    }
    for(unsigned malformed=0;malformed<7;++malformed) {
        XrXirMapLiteral saved=literal;XrXirMapLiteralOrigin origin=items[0];
        if(malformed==0)literal.file.bytes=NULL;
        if(malformed==1)literal.file=(XrXirLiteral){"a\0b",3};
        if(malformed==2)literal.items=NULL;
        if(malformed==3)literal.module=1;
        if(malformed==4)items[0].kind=2;
        if(malformed==5)items[0].line=0;
        if(malformed==6)items[0].column=0;
        CHECK(xr_xir_compile_declarations_verify(&compile,&source,NULL,2,XR_XIR_PROGRAM, NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_xir_compile_declarations_clone(&compile,&source,2,NULL,&copy)==XR_XIR_BAD_STRUCTURE && !copy);
        literal=saved;items[0]=origin;
    }
    XrXirCompileContext bounded=compile;bounded.limits.parameters=1;
    CHECK(xr_xir_compile_declarations_clone(&bounded,&source,2,NULL,&copy)==XR_XIR_BUDGET && !copy);
    CHECK(xr_xir_compile_declarations_clone(&compile,&source,2,NULL,&copy)==XR_XIR_OK);
    memset(file,'x',sizeof(file));memset(items,0xa5,sizeof(items));memset(&literal,0xa5,sizeof(literal));
    CHECK(copy->map_literal_count==1 && copy->map_literals[0].file.length==6 &&
        !memcmp(copy->map_literals[0].file.bytes,"app.xr",6) && copy->map_literals[0].count==2 &&
        copy->map_literals[0].items[0].kind==0 && copy->map_literals[0].items[0].line==3 &&
        copy->map_literals[0].items[0].column==12 && copy->map_literals[0].items[1].kind==1 &&
        copy->map_literals[0].items[1].line==4 && copy->map_literals[0].items[1].column==9);
    CHECK(xr_xir_compile_declarations_verify(&compile,copy,NULL,2,XR_XIR_PROGRAM, NULL)==XR_XIR_OK);
    xr_xir_compile_declarations_free(copy);xr_compile_resources_release(resources);
    CHECK(!physical_live && !physical_bytes);
    map_literal_packet_owners();
    map_library_constant_owners();
}
