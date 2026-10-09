/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "base/xmalloc.h"
#include "app/lsp/xlsp_source_query.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"

static const char source_root_text[] =
    "import { value } from \"./lib\"\n"
    "export fn result()->i64 { /* \xf0\x9f\x98\x80 */ return value() }\n";
static const char source_library_text[] =
    "/* \xf0\x9f\x98\x80 */ export fn value()->i64 { return 41 }\n";
static XrCompileResourceLimits lsp_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
static void lsp_zero(void) { CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes); }
static void lsp_uri(char *output, size_t capacity, const char *root, const char *name) {
#if defined(_WIN32)
    int length = snprintf(output, capacity, "file:///%s/%s", root, name);
#else
    int length = snprintf(output, capacity, "file://%s/%s", root, name);
#endif
    CHECK(length > 0 && (size_t)length < capacity);
    for (int i = 0; i < length; ++i) if (output[i] == '\\') output[i] = '/';
}
static XrXirStatus lsp_facts(XlspSourceSnapshot *snapshot, const char *root_uri,
    const char *library_uri, uint32_t line, int64_t version) {
    /* Fixed from literal UTF-16 text, independently of Source output:
     * root call: (1,42)..(1,47); library name: (line,19)..(line,24).
     * Their UTF-8 columns are two bytes greater due to one non-BMP scalar. */
    XlspSourceLocation definition = {0};
    XrXirStatus status = xlsp_source_definition(snapshot, root_uri, strlen(root_uri),
        (XrLspPosition){1,43}, &definition);
    if (status != XR_XIR_OK) return status;
    CHECK(!strcmp(definition.uri, library_uri) && definition.version == version);
    CHECK(definition.range.start.line == line && definition.range.end.line == line);
    CHECK(definition.range.start.character == 19 && definition.range.end.character == 24);
    XlspSourceLocations references = {0};
    status = xlsp_source_references(snapshot, library_uri, strlen(library_uri),
        (XrLspPosition){line,20}, false, &references);
    if (status != XR_XIR_OK) return status;
    CHECK(references.count == 1 && !strcmp(references.items[0].uri, root_uri));
    CHECK(references.items[0].range.start.line == 1 && references.items[0].range.end.line == 1);
    CHECK(references.items[0].range.start.character == 42 && references.items[0].range.end.character == 47);
    xlsp_source_locations_free(&references);
    status = xlsp_source_references(snapshot, library_uri, strlen(library_uri),
        (XrLspPosition){line,20}, true, &references);
    if (status != XR_XIR_OK) return status;
    CHECK(references.count == 2 && !strcmp(references.items[0].uri,library_uri));
    CHECK(references.items[0].range.start.line == line && references.items[0].range.start.character == 19);
    CHECK(!strcmp(references.items[1].uri,root_uri) && references.items[1].range.start.character == 42);
    xlsp_source_locations_free(&references);
    XlspSourceHover hover={0};
    status=xlsp_source_hover(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,43},&hover);
    if(status!=XR_XIR_OK){CHECK(!hover.text&&!hover.length);return status;}
    CHECK(hover.length==sizeof("export fn value() -> i64")-1&&!strcmp(hover.text,"export fn value() -> i64"));
    XlspSourceHover occupied=hover;
    CHECK(xlsp_source_hover(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,43},&occupied)==XR_XIR_BAD_STRUCTURE&&occupied.text==hover.text);
    xlsp_source_hover_free(&hover);return XR_XIR_OK;
}
static XrXirStatus lsp_once(const char *root, XrCompileResourceLimits limits, XrCompileResourceStats *stats) {
    XrXirCompileContext context = {0}; XrCompilerSession *session = NULL; XlspSourceSnapshot *snapshot = NULL;
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK) return resource == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSessionStatus created = xr_compile_session_new(context.resources, &session);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    char root_uri[XR_TEST_PATH_MAX], library_uri[XR_TEST_PATH_MAX];
    char root_text[sizeof(source_root_text)], library_text[sizeof(source_library_text)];
    memcpy(root_text, source_root_text, sizeof(root_text)); memcpy(library_text, source_library_text, sizeof(library_text));
    lsp_uri(root_uri, sizeof(root_uri), root, "root.xr"); lsp_uri(library_uri, sizeof(library_uri), root, "%6cib.xr");
    XlspSourceDocument documents[2] = {
        {root_uri,root_text,strlen(root_uri),sizeof(root_text)-1,1},
        {library_uri,library_text,strlen(library_uri),sizeof(library_text)-1,7}};
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    if (status == XR_XIR_OK) status = xlsp_source_snapshot_build(&request,documents,2,0,&snapshot,NULL,NULL);
    memset(root_text,'?',sizeof(root_text)); memset(library_text,'?',sizeof(library_text));
    memset(documents,0,sizeof(documents)); memset(&request,0,sizeof(request));
    xr_compile_session_free(session);
    /* Snapshot allocations pin the ledger. Producer Session and caller ledger
     * lease are gone before any query below. Keep a separate observation pin. */
    CHECK(xr_compile_resources_retain(context.resources) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);
    if (status == XR_XIR_OK) status = lsp_facts(snapshot,root_uri,library_uri,0,7);
    if (stats) CHECK(xr_compile_resources_stats(context.resources,stats) == XR_COMPILE_RESOURCE_OK);
    xlsp_source_snapshot_free(snapshot); xr_compile_resources_release(context.resources);
    return status;
}
static void lsp_failure_matrix(const char *root) {
    size_t sites = 0; XrCompileResourceStats required = {0};
    for (size_t probe = 0; probe <= sites; ++probe) {
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = probe ? probe-1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrXirStatus status = lsp_once(root,lsp_limits(),probe ? NULL : &required);
        if (!probe) { CHECK(status == XR_XIR_OK); sites = source_fixture_compile_attempts; CHECK(sites); }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
        lsp_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected = false;
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound < UINT64_MAX); *bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(lsp_once(root,limits,NULL) == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK)); lsp_zero();
    }
    printf("LSP real Source/query fresh sites=%zu; three axes; physical=0/0\n",sites);
}
static void lsp_versions(const char *root) {
    XrCompileResourceLimits limits = lsp_limits(); XrXirCompileContext context = {0}; XrCompilerSession *session = NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits(); CHECK(xr_compile_session_new(context.resources,&session) == XR_COMPILER_SESSION_OK);
    char root_uri[XR_TEST_PATH_MAX], library_uri[XR_TEST_PATH_MAX], next[sizeof(source_library_text)+1];
    lsp_uri(root_uri,sizeof(root_uri),root,"root.xr"); lsp_uri(library_uri,sizeof(library_uri),root,"lib.xr");
    next[0]='\n'; memcpy(next+1,source_library_text,sizeof(source_library_text));
    XlspSourceDocument documents[2] = {
        {root_uri,source_root_text,strlen(root_uri),sizeof(source_root_text)-1,1},
        {library_uri,source_library_text,strlen(library_uri),sizeof(source_library_text)-1,7}};
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceSnapshot *old = NULL, *updated = NULL; bool matches = false;
    CHECK(xlsp_source_snapshot_build(&request,documents,2,0,&old,NULL,NULL) == XR_XIR_OK);
    CHECK(xlsp_source_snapshot_matches(old,documents,2,0,&matches) == XR_XIR_OK && matches);
    documents[1].version = 8;
    CHECK(xlsp_source_snapshot_matches(old,documents,2,0,&matches) == XR_XIR_OK && !matches);
    documents[1].version = 7; documents[1].text = next; ++documents[1].length;
    CHECK(xlsp_source_snapshot_matches(old,documents,2,0,&matches) == XR_XIR_OK && !matches);
    documents[1].version = 8;
    CHECK(xlsp_source_snapshot_build(&request,documents,2,0,&updated,NULL,NULL) == XR_XIR_OK);
    CHECK(xlsp_source_snapshot_matches(updated,documents,2,0,&matches) == XR_XIR_OK && matches);
    memset(next,'?',sizeof(next)); memset(documents,0,sizeof(documents));
    xr_compile_session_free(session); xr_compile_resources_release(context.resources);
    CHECK(lsp_facts(old,root_uri,library_uri,0,7) == XR_XIR_OK);
    CHECK(lsp_facts(updated,root_uri,library_uri,1,8) == XR_XIR_OK);
    XlspSourceLocation unchanged = {"sentinel",{{91,92},{93,94}},99,XR_XIR_SOURCE_READ};
    CHECK(xlsp_source_definition(old,root_uri,strlen(root_uri),(XrLspPosition){1,30},&unchanged) == XR_XIR_BAD_STRUCTURE);
    CHECK(!strcmp(unchanged.uri,"sentinel") && unchanged.version == 99 && unchanged.range.start.line == 91);
    xlsp_source_snapshot_free(old);
    CHECK(lsp_facts(updated,root_uri,library_uri,1,8) == XR_XIR_OK);
    xlsp_source_snapshot_free(updated); lsp_zero();
}
static void lsp_rejections(const char *root) {
    const char *tails[] = {"bad%00.xr","bad%2fchild.xr","bad%.xr","../escape.xr","bad.xr?query","bad.xr#fragment","bad%ff.xr"};
    for (size_t mode = 0; mode < sizeof(tails)/sizeof(tails[0]); ++mode) {
        XrCompileResourceLimits limits = lsp_limits(); XrXirCompileContext context = {0}; XrCompilerSession *session = NULL;
        CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits(); CHECK(xr_compile_session_new(context.resources,&session) == XR_COMPILER_SESSION_OK);
        char uri[XR_TEST_PATH_MAX]; lsp_uri(uri,sizeof(uri),root,tails[mode]);
        XlspSourceDocument document={uri,"",strlen(uri),0,1};
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
        XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XlspSourceSnapshot *snapshot=NULL;
        CHECK(xlsp_source_snapshot_build(&request,&document,1,0,&snapshot,NULL,NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(!snapshot); xr_compile_session_free(session); xr_compile_resources_release(context.resources); lsp_zero();
    }
}
#include "lsp_source_json_cases.h"
#include "lsp_source_navigation_many_cases.h"
#include "lsp_source_syntax_cases.h"
#include "lsp_source_buffer_cases.h"
#include "lsp_source_hover_cases.h"
#include "lsp_source_outline_cases.h"
#include "lsp_source_semantic_cases.h"
#include "lsp_source_roles_cases.h"
int main(void) {
    char directory[XR_TEST_PATH_MAX]="lsp-source-query-XXXXXX",root[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,root,sizeof(root)));
    lsp_versions(root); lsp_rejections(root); lsp_failure_matrix(root); lsp_json_cases(root); lsp_navigation_many_cases(root); lsp_syntax_cases(root); lsp_buffer_cases(); lsp_hover_cases(root); lsp_outline_cases(root); lsp_semantic_cases(root); lsp_roles_cases(root); CHECK(xr_test_rmdir(directory) == 0);
    puts("LSP unsaved two-document navigation, same-Session versions, Unicode, producer death passed");
    return 0;
}
