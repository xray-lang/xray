/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_imports.h"
#include "xlsp_source_syntax.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../base/xarena.h"
#include "../../shared/xr_utf8_core.h"
#include <limits.h>
#include <string.h>

struct XlspSourceImports {XlspSourceImport *items;size_t count;};
static XrXirStatus imports_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus imports_work(XrCompileResources *r,size_t amount) {
    return imports_resource(xr_compile_resources_work(r,amount));
}
static int imports_read(void *context,const uint8_t *address,uint8_t *out) {
    if(imports_work(context,1)!=XR_XIR_OK)return 0;*out=*address;return 1;
}
XR_FUNC void xlsp_source_imports_free(XlspSourceImports *owner) {
    if(!owner)return;
    for(size_t i=0;i<owner->count;++i)xr_compile_resources_free((char *)owner->items[i].specifier);
    xr_compile_resources_free(owner->items);xr_compile_resources_free(owner);
}
XR_FUNC const XlspSourceImport *xlsp_source_imports_items(const XlspSourceImports *owner,size_t *count) {
    if(count)*count=owner?owner->count:0;return owner?owner->items:NULL;
}
static XrXirStatus imports_copy(XrCompileResources *r,const char *text,char **out) {
    size_t length=0;
    for(;;++length) {
        XrXirStatus status=imports_work(r,1);if(status!=XR_XIR_OK)return status;
        if(!text[length])break;if(length==SIZE_MAX-1)return XR_XIR_BUDGET;
    }
    XrXirStatus status=imports_resource(xr_compile_resources_alloc(r,length+1,(void **)out));
    if(status==XR_XIR_OK)status=imports_work(r,length+1);
    if(status==XR_XIR_OK)memcpy(*out,text,length+1);return status;
}
static XrXirStatus imports_position(XrCompileResources *r,const XlspSourceBuffer *buffer,size_t offset,XrLspPosition *out) {
    size_t length=xlsp_source_buffer_length(buffer);if(offset>length)return XR_XIR_BAD_STRUCTURE;
    int count=0;const uint32_t *lines=xlsp_source_buffer_lines(buffer,&count);int line=0;
    while(line+1<count) {
        XrXirStatus status=imports_work(r,1);if(status!=XR_XIR_OK)return status;
        if(lines[line+1]>offset)break;++line;
    }
    const char *text=xlsp_source_buffer_text(buffer);uint32_t column=0;
    for(size_t i=lines[line];i<offset;) {
        XrUtf8Step step;
        if(!xr_utf8_core_decode_step_read((const uint8_t *)text+i,offset-i,imports_read,r,&step))return XR_XIR_BUDGET;
        if(step.error!=XR_UTF8_OK||!step.scalar||step.scalar=='\r'||step.scalar=='\n')return XR_XIR_BAD_STRUCTURE;
        uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
        if(column>INT32_MAX-width)return XR_XIR_BAD_STRUCTURE;
        column+=width;i+=step.consumed;
    }
    *out=(XrLspPosition){(uint32_t)line,column};return XR_XIR_OK;
}
/* Parser rows follow LF; logical LSP rows are derived separately from the
 * exact token offsets, so lone CR and CRLF never mix the two coordinate systems. */
static XrXirStatus imports_offset(XrCompileResources *r,const char *text,size_t length,const AstNode *node,size_t *out) {
    if(node->line<=0||node->column<=0)return XR_XIR_UNSUPPORTED;
    size_t offset=0;int line=1;
    while(offset<length&&line<node->line) {
        XrXirStatus status=imports_work(r,1);if(status!=XR_XIR_OK)return status;
        if(text[offset++]=='\n')++line;
    }
    if(line!=node->line||(size_t)(node->column-1)>length-offset)return XR_XIR_BAD_STRUCTURE;
    offset+=(size_t)(node->column-1);if(offset==length)return XR_XIR_BAD_STRUCTURE;
    *out=offset;return XR_XIR_OK;
}
static bool imports_from(Token token) {
    return token.type==TK_NAME&&token.length==4&&!memcmp(token.start,"from",4);
}
static XrXirStatus imports_token(XrCompilerSession *session,const XlspSourceBuffer *buffer,const AstNode *node,Token *out) {
    XrCompileState *state=xr_compile_session_compile_state(session);XrCompileResources *r=xr_compile_state_resources(state);
    const char *text=xlsp_source_buffer_text(buffer);size_t offset=0;
    XrXirStatus status=imports_offset(r,text,xlsp_source_buffer_length(buffer),node,&offset);if(status!=XR_XIR_OK)return status;
    Scanner scanner={0};XrArena arena={0};XrArenaBacking backing;
    if(xr_compiler_arena_state_backing(state,&backing)!=XR_ARENA_OK)return XR_XIR_BAD_STRUCTURE;
    (void)xr_arena_open(&arena,XR_ARENA_SEGMENT_SIZE,&backing);
    status=imports_resource(xr_compiler_arena_capture_status(&arena,state));
    if(status==XR_XIR_OK)status=imports_resource(xr_compile_scanner_open(&scanner,state,&arena,text+offset,NULL));
    if(status!=XR_XIR_OK)goto done;
    Token token=xr_scanner_scan(&scanner);
    if(token.start!=text+offset||token.type!=(node->type==AST_IMPORT_STMT?TK_IMPORT:TK_EXPORT)){status=XR_XIR_BAD_STRUCTURE;goto done;}
    token=xr_scanner_scan(&scanner);
    if(token.type==TK_LBRACE) {
        /* Only inspect this parser-admitted member list, never executable
         * expressions where regex tokenization needs parser context. */
        do {token=xr_scanner_scan(&scanner);}while(token.type!=TK_RBRACE&&token.type!=TK_ERROR&&token.type!=TK_EOF);
        if(token.type!=TK_RBRACE){status=XR_XIR_BAD_STRUCTURE;goto done;}
        token=xr_scanner_scan(&scanner);
        status=imports_work(r,4);if(status!=XR_XIR_OK)goto done;
        if(!imports_from(token)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        token=xr_scanner_scan(&scanner);
    } else if(node->type==AST_EXPORT_STMT&&token.type==TK_STAR) {
        token=xr_scanner_scan(&scanner);
        status=imports_work(r,4);if(status!=XR_XIR_OK)goto done;
        if(!imports_from(token)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        token=xr_scanner_scan(&scanner);
    }
    if(token.type!=TK_NAME&&token.type!=TK_LITERAL_STRING){status=XR_XIR_BAD_STRUCTURE;goto done;}
    if(token.length<=0||token.start<text||(size_t)(token.start-text)>xlsp_source_buffer_length(buffer)||
       (size_t)token.length>xlsp_source_buffer_length(buffer)-(size_t)(token.start-text)){status=XR_XIR_BAD_STRUCTURE;goto done;}
    *out=token;
done:
    {XrCompileResourceStatus failed=xr_compile_state_status(state);if(failed!=XR_COMPILE_RESOURCE_OK)status=imports_resource(failed);}
    xr_arena_destroy(&arena);return status;
}
XR_FUNC XrXirStatus xlsp_source_imports_build(XrCompileResources *r,const XlspSourceDocument *document,XlspSourceImports **out) {
    if(!r||!document||!document->text||!document->uri||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceImports *owner=NULL;XlspSourceBuffer *buffer=NULL;XlspSyntaxSnapshot *syntax=NULL;XrCompilerSession *session=NULL;
    XrXirStatus status=xlsp_source_buffer_new(r,document->text,document->length,0,&buffer);if(status!=XR_XIR_OK)goto done;
    XrCompileResourceStatus opened=xr_compile_session_new(r,&session);
    if(opened!=XR_COMPILE_RESOURCE_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XrParseStatus parsed=xlsp_source_syntax_build(session,document,&syntax);
    if(parsed!=XR_PARSE_OK){status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        parsed==XR_PARSE_SYNTAX||parsed==XR_PARSE_RECOVERED?XR_XIR_UNRESOLVED:XR_XIR_BAD_STRUCTURE;goto done;}
    const AstNode *root=xlsp_source_syntax_ast(syntax);
    if(!root||root->type!=AST_PROGRAM||root->as.program.count<0){status=XR_XIR_BAD_STRUCTURE;goto done;}
    size_t capacity=(size_t)root->as.program.count;if(capacity>SIZE_MAX/sizeof(XlspSourceImport)){status=XR_XIR_BUDGET;goto done;}
    status=imports_resource(xr_compile_resources_calloc(r,1,sizeof(*owner),(void **)&owner));if(status!=XR_XIR_OK)goto done;
    if(capacity){status=imports_resource(xr_compile_resources_calloc(r,capacity,sizeof(*owner->items),(void **)&owner->items));if(status!=XR_XIR_OK)goto done;}
    for(size_t i=0;i<capacity;++i) {
        status=imports_work(r,sizeof(AstNode *));if(status!=XR_XIR_OK)goto done;
        const AstNode *node=root->as.program.statements[i];if(!node)continue;
        const char *name=NULL;bool quoted=false;
        if(node->type==AST_IMPORT_STMT){name=node->as.import_stmt.module_name;quoted=node->as.import_stmt.is_quoted;}
        else if(node->type==AST_EXPORT_STMT){name=node->as.export_stmt.from_path;quoted=node->as.export_stmt.from_is_quoted;}
        if(!name)continue;
        Token token={0};status=imports_token(session,buffer,node,&token);if(status!=XR_XIR_OK)goto done;
        if((token.type==TK_LITERAL_STRING)!=quoted){status=XR_XIR_BAD_STRUCTURE;goto done;}
        size_t begin=(size_t)(token.start-xlsp_source_buffer_text(buffer));XrLspRange range={0};
        status=imports_position(r,buffer,begin,&range.start);if(status!=XR_XIR_OK)goto done;
        status=imports_position(r,buffer,begin+(size_t)token.length,&range.end);if(status!=XR_XIR_OK)goto done;
        char *copy=NULL;status=imports_copy(r,name,&copy);
        if(status==XR_XIR_OK)status=imports_work(r,sizeof(XlspSourceImport));
        if(status!=XR_XIR_OK){xr_compile_resources_free(copy);goto done;}
        owner->items[owner->count++]=(XlspSourceImport){copy,range,quoted};
    }
    *out=owner;owner=NULL;
done:
    xlsp_source_imports_free(owner);xlsp_source_syntax_free(syntax);xr_compile_session_free(session);xlsp_source_buffer_free(buffer);return status;
}
