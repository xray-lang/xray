/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_emit_fixture.h - Build fixture production without running unit matrices
 *
 * KEY CONCEPT:
 *   Fixture generation preserves genuine Library import without running fault matrices.
 */
static void library_emit_fixture(const char *path) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    library_context=&owner.context;
    conflict_text("library.xr","fn secret(value:i64=41)->i64{return value;}\nexport fn answer(value:i64=secret())->i64{return value;}\n");
    conflict_text("root.xr","import {answer} from \"./library\";\nexport fn result()->i64{return answer();}\n");
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrCompilerSession *session=library_session_new(library_context);CHECK(session);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/library.xr",&authority,library_context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult library={0},program={0};CHECK(xr_xir_compile_source_check(&request,&library,NULL,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(library.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&library);xr_compile_session_free(session);
    XrXirLibraryModuleInput binding={authority,"library.xr"};XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);session=library_session_new(library_context);CHECK(session);
    request=(XrXirSourceRequest){session,XR_SOURCE_FIXTURES "/root.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    CHECK(xr_xir_compile_source_check(&request,&program,NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_xir_compile_checked_write(program.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&program);
    FILE *file=fopen(path,"wb");CHECK(file && packet.length<=262144);
    CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    xr_xir_compile_checked_packet_free(&packet);library_compile_owner_drop(&owner);
    library_context=NULL;library_compile_observer_free();CHECK(!runtime_live&&!runtime_bytes);
}
