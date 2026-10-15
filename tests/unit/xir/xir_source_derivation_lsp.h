/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_lsp.h - Owned observations of generated methods
 */
#ifndef XIR_SOURCE_DERIVATION_LSP_H
#define XIR_SOURCE_DERIVATION_LSP_H
#include "app/lsp/xlsp_source_query.h"
static void derive_lsp(const XrXirCompileContext *context,const char *path,const XrXirLibraryCatalog *catalog) {
    char uri[2200],query_uri[2200],text[sizeof(user_source)];
    int n=snprintf(uri,sizeof(uri),"file:///%s",path);CHECK(n>0 && n<(int)sizeof(uri));
    memcpy(query_uri,uri,(size_t)n+1);memcpy(text,user_source,sizeof(text));
    XlspSourceDocument document={uri,text,(size_t)n,sizeof(text)-1,41};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_WORK};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog,NULL};
    XlspSourceSnapshot *snapshot=NULL;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xlsp_source_snapshot_build(&request,&document,1,0,&snapshot,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"derive LSP build=%u %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_XIR_OK && snapshot);
    xr_compile_session_free(session);memset(uri,0xa5,sizeof(uri));memset(text,0xa5,sizeof(text));memset(&document,0xa5,sizeof(document));
    const char *call=strstr(user_source,"value.measure");CHECK(call);call+=6;
    XrLspPosition position={0};
    for (const char *p=user_source;p<call;++p) {
        if (*p=='\n') {++position.line;position.character=0;} else ++position.character;
    }
    XlspSourceLocation location={0};
    CHECK(xlsp_source_definition(snapshot,query_uri,(size_t)n,position,&location)==XR_XIR_OK);
    CHECK(location.version==41 && !strcmp(location.uri,query_uri) && location.range.start.line==2 &&
        location.range.start.character==1 && location.range.end.line==2 && location.range.end.character==7);
    XlspSourceHover hover={0};status=xlsp_source_hover(snapshot,query_uri,(size_t)n,position,&hover);
    if (status!=XR_XIR_OK) fprintf(stderr,"derive LSP hover=%u\n",(unsigned)status);
    CHECK(status==XR_XIR_OK && hover.text && strstr(hover.text,"fn Future.measure() -> i64"));
    xlsp_source_hover_free(&hover);XlspSourceLocations references={0};
    CHECK(xlsp_source_references(snapshot,query_uri,(size_t)n,position,true,&references)==XR_XIR_OK && references.count==3);
    xlsp_source_locations_free(&references);XlspSourceTokens *tokens=NULL;
    status=xlsp_source_semantic_tokens(snapshot,query_uri,(size_t)n,&tokens);
    if (status!=XR_XIR_OK) fprintf(stderr,"derive LSP tokens=%u\n",(unsigned)status);
    CHECK(status==XR_XIR_OK && tokens);size_t count=0;const XlspSourceToken *rows=xlsp_source_tokens_items(tokens,&count);bool found=false;
    for (size_t i=0;i<count;++i) {
        CHECK(rows[i].line!=2 || rows[i].column!=1);
        if (rows[i].line==position.line && rows[i].column==position.character) {
            CHECK(rows[i].type==13 && rows[i].length==7);found=true;
        }
    }
    CHECK(found);xlsp_source_tokens_free(tokens);xlsp_source_snapshot_free(snapshot);
}
#endif // XIR_SOURCE_DERIVATION_LSP_H
