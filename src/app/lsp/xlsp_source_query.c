/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_query.h"
#include "xlsp_source_signature.h"
#include "../../xir/xxir_source_calls.h"
#include "../../module/xmodule_overlay.h"
#include "../../shared/xr_utf8_core.h"
#include "../../xir/xxir_compile_memory.h"
#include "../../toolchain/xcompiler_session.h"
#include <limits.h>
#include <string.h>

typedef struct LspSourceText {
    char *uri, *text, *path, *logical, *identity;
    size_t uri_length, length;
    int64_t version;
    uint32_t module;
    uint32_t *modules;size_t module_count;
} LspSourceText;
struct XlspSourceSnapshot {
    XrCompileResources *resources; /* The allocated owner itself pins this ledger. */
    LspSourceText *texts;
    size_t count, entry;
    XrXirSourceSnapshot *query;
};
XR_FUNC bool xlsp_source_snapshot_uses_resources(const XlspSourceSnapshot *s,const XrCompileResources *resources) {
    return s&&resources&&s->resources==resources;
}
static XrXirStatus lsp_copy(const XlspSourceSnapshot *s, const char *bytes, size_t length, char **out) {
    if (!bytes || length == SIZE_MAX || memchr(bytes, 0, length)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xir_compile_resource_status(xr_compile_resources_alloc(s->resources, length + 1, (void **)out));
    if (status != XR_XIR_OK) return status;
    memcpy(*out, bytes, length); (*out)[length] = 0;
    return XR_XIR_OK;
}
static XrXirStatus lsp_equal(const char *a, const char *b, size_t length, bool *equal) {
    *equal = !memcmp(a, b, length);
    return XR_XIR_OK;
}
static XrXirStatus lsp_path_equal(const char *a,const char *b,bool *out) {
    if(!a||!b){*out=false;return XR_XIR_OK;}
    for(;;++a,++b) {
        char ac=*a,bc=*b;
#if defined(_WIN32)
        if(ac=='\\')ac='/';
        if(bc=='\\')bc='/';
#endif
        if(ac!=bc){*out=false;return XR_XIR_OK;}
        if(!ac){*out=true;return XR_XIR_OK;}
    }
}
static XrXirStatus lsp_text_has_module(const LspSourceText *d,uint32_t module,bool *out) {
    for(size_t i=0;i<d->module_count;++i) {
        if(d->modules[i]==module){*out=true;return XR_XIR_OK;}
    }
    *out=false;return XR_XIR_OK;
}
static int lsp_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* URI syntax is decoded once, before authoritative identity construction.
 * No filesystem case, symlink, device-path or path-equivalence guesses occur. */
static XrXirStatus lsp_uri_path(const XlspSourceSnapshot *s, LspSourceText *d) {
    if (d->uri_length < 8) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status;
    if (memcmp(d->uri, "file://", 7)) return XR_XIR_BAD_STRUCTURE;
    size_t start = 7, host_end = start;
    while (host_end < d->uri_length) {
        unsigned char c = (unsigned char)d->uri[host_end];
        if (c == '/') break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.')) return XR_XIR_BAD_STRUCTURE;
        ++host_end;
    }
    if (host_end == d->uri_length) return XR_XIR_BAD_STRUCTURE;
    bool local = host_end == start;
    if (host_end - start == 9) {
        local = !memcmp(d->uri + start, "localhost", 9);
    }
    status = xir_compile_resource_status(xr_compile_resources_alloc(s->resources, d->uri_length + 1, (void **)&d->path));
    if (status != XR_XIR_OK) return status;
    size_t used = 0;
    if (!local) {
#if defined(_WIN32)
        d->path[used++] = '/'; d->path[used++] = '/';
        memcpy(d->path + used, d->uri + start, host_end - start); used += host_end - start;
#else
        return XR_XIR_UNSUPPORTED;
#endif
    }
    for (size_t i = host_end; i < d->uri_length; ++i) {
        unsigned char c = (unsigned char)d->uri[i];
        if (c == '?' || c == '#' || c == '\\' || c <= 32 || c == 127) return XR_XIR_BAD_STRUCTURE;
        if (c == '%') {
            if (d->uri_length - i < 3) return XR_XIR_BAD_STRUCTURE;
            int high = lsp_hex((unsigned char)d->uri[i+1]), low = lsp_hex((unsigned char)d->uri[i+2]);
            if (high < 0 || low < 0) return XR_XIR_BAD_STRUCTURE;
            c = (unsigned char)((high << 4) | low); i += 2;
            /* Encoded separators cannot change URI path structure. */
            if (!c || c == '/' || c == '\\' || c < 32 || c == 127) return XR_XIR_BAD_STRUCTURE;
        }
        d->path[used++] = (char)c;
    }
#if defined(_WIN32)
    if (local && used >= 3) {
        unsigned char drive = (unsigned char)d->path[1];
        if (d->path[0] == '/' && d->path[2] == ':' &&
            ((drive >= 'a' && drive <= 'z') || (drive >= 'A' && drive <= 'Z'))) {
            memmove(d->path, d->path + 1, --used);
        }
    }
#endif
    for (size_t offset = 0; offset < used;) {
        XrUtf8Step step = xr_utf8_core_decode_step((const uint8_t *)d->path + offset, used - offset);
        if (step.error != XR_UTF8_OK) return XR_XIR_BAD_STRUCTURE;
        offset += step.consumed;
    }
    d->path[used] = 0;
    return XR_XIR_OK;
}
/* Match either a UTF-16 cursor or a Source byte cursor by walking strict scalars.
 * Source columns are one-based bytes; LSP columns are zero-based UTF-16 units. */
static XrXirStatus lsp_position_at(const LspSourceText *d,
    bool from_lsp, XrLspPosition *lsp, int *line, int *column, size_t *byte_offset) {
    uint32_t row = 0, source_row = 0, units = 0; size_t byte_column = 0, offset = 0;
    for (;;) {
        if (from_lsp ? (row == lsp->line && units == lsp->character) :
            (*line > 0 && *column > 0 && source_row == (uint32_t)(*line - 1) && byte_column == (size_t)(*column - 1))) {
            if (source_row >= INT_MAX || byte_column >= INT_MAX) return XR_XIR_BAD_STRUCTURE;
            if (from_lsp) { *line = (int)source_row + 1; *column = (int)byte_column + 1; }
            else *lsp = (XrLspPosition){row, units};
            if(byte_offset)*byte_offset=offset;
            return XR_XIR_OK;
        }
        if (offset == d->length) return XR_XIR_BAD_STRUCTURE;
        XrUtf8Step step = xr_utf8_core_decode_step((const uint8_t *)d->text + offset, d->length - offset);
        if (step.error != XR_UTF8_OK || !step.scalar) return XR_XIR_BAD_STRUCTURE;
        if (step.scalar == '\r') {
            /* CRLF is one newline; neither code unit is a selectable column. */
            if (offset + 1 < d->length) {
                if (d->text[offset + 1] == '\n') ++step.consumed;
            }
            if (row == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            ++row; units = 0;
            if (step.consumed == 2) { ++source_row; byte_column = 0; }
            else ++byte_column;
        } else if (step.scalar == '\n') {
            if (row == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            ++row; ++source_row; units = 0; byte_column = 0;
        } else {
            uint32_t width = step.scalar > UINT32_C(0xffff) ? 2u : 1u;
            if (units > UINT32_MAX - width) return XR_XIR_BAD_STRUCTURE;
            units += width; byte_column += step.consumed;
        }
        offset += step.consumed;
        if (from_lsp && (row > lsp->line || (row == lsp->line && units > lsp->character))) return XR_XIR_BAD_STRUCTURE;
        if (!from_lsp && (*line <= 0 || *column <= 0 || source_row > (uint32_t)(*line - 1) ||
            (source_row == (uint32_t)(*line - 1) && byte_column > (size_t)(*column - 1)))) return XR_XIR_BAD_STRUCTURE;
    }
}
static XrXirStatus lsp_position(const LspSourceText *d,
    bool from_lsp,XrLspPosition *lsp,int *line,int *column) {
    return lsp_position_at(d,from_lsp,lsp,line,column,NULL);
}
void xlsp_source_snapshot_free(XlspSourceSnapshot *s) {
    if (!s) return;
    xr_xir_compile_source_snapshot_free(s->query);
    for (size_t i = 0; i < s->count; ++i) {
        LspSourceText *d = &s->texts[i];
        xr_compile_resources_free(d->modules);
        xr_compile_resources_free(d->uri); xr_compile_resources_free(d->text);
        xr_compile_resources_free(d->path); xr_compile_resources_free(d->logical);
        xr_compile_resources_free(d->identity);
    }
    xr_compile_resources_free(s->texts); xr_compile_resources_free(s);
}
XrXirStatus xlsp_source_snapshot_build_authorities(const XrXirSourceRequest *request,
    const XlspSourceDocument *documents, const XrModuleIdentityAuthority *authorities, size_t count, size_t entry,
    XlspSourceSnapshot **output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure) {
    if (!request || !request->context || !request->context->resources || !request->session ||
        !request->authority || request->authority->kind == XR_MODULE_IDENTITY_MEMORY ||
        !documents || !count || entry >= count || !output || *output || !xr_xir_compile_source_failure_empty(failure) ||
        count > SIZE_MAX / sizeof(LspSourceText) || count > SIZE_MAX / sizeof(XrModuleOverlayInput)) return XR_XIR_BAD_STRUCTURE;
    XlspSourceSnapshot *s = NULL; XrModuleOverlayInput *inputs = NULL; XrXirSourceResult result = {0};
    XrXirStatus status = xir_compile_resource_status(xr_compile_session_poll(request->session));
    if (status != XR_XIR_OK) return status;
    status = xir_compile_resource_status(xr_compile_resources_calloc(request->context->resources, 1, sizeof(*s), (void **)&s));
    if (status != XR_XIR_OK) return status;
    s->resources = request->context->resources; s->entry = entry;
    status = xir_compile_resource_status(xr_compile_resources_calloc(s->resources, count, sizeof(*s->texts), (void **)&s->texts));
    if (status != XR_XIR_OK) goto done;
    s->count = count;
    status = xir_compile_resource_status(xr_compile_resources_calloc(s->resources, count, sizeof(*inputs), (void **)&inputs));
    if (status != XR_XIR_OK) goto done;
    for (size_t i = 0; i < count; ++i) {
        status = xir_compile_resource_status(xr_compile_session_poll(request->session));
        if (status != XR_XIR_OK) goto done;
        const XlspSourceDocument *input = &documents[i]; LspSourceText *d = &s->texts[i];
        d->module = UINT32_MAX; d->uri_length = input->uri_length; d->length = input->length; d->version = input->version;
        status = lsp_copy(s, input->uri, input->uri_length, &d->uri); if (status != XR_XIR_OK) goto done;
        status = lsp_copy(s, input->text, input->length, &d->text); if (status != XR_XIR_OK) goto done;
        status = lsp_uri_path(s, d); if (status != XR_XIR_OK) goto done;
        XrModuleStatus identity = xr_compile_module_identity_from_source(s->resources, authorities ? &authorities[i] : request->authority,
            d->path, &d->identity, &d->logical);
        if (identity != XR_MODULE_OK) {
            switch (identity) {
            case XR_MODULE_BUDGET: status = XR_XIR_BUDGET; break;
            case XR_MODULE_OUT_OF_MEMORY: status = XR_XIR_OUT_OF_MEMORY; break;
            case XR_MODULE_MEMORY_LIMIT: status = XR_XIR_MEMORY_LIMIT; break;
            case XR_MODULE_OVERFLOW: status = XR_XIR_OVERFLOW; break;
            case XR_MODULE_CANCELLED: status = XR_XIR_CANCELLED; break;
            case XR_MODULE_DEADLINE: status = XR_XIR_DEADLINE; break;
            case XR_MODULE_CLOCK_FAILURE: status = XR_XIR_CLOCK_FAILURE; break;
            default: status = XR_XIR_BAD_STRUCTURE; break;
            }
            goto done;
        }
        inputs[i] = (XrModuleOverlayInput){authorities ? authorities[i] : *request->authority, d->logical, d->path, d->text, d->length};
    }
    for(size_t i=0;i<count;++i)for(size_t j=0;j<i;++j) {
        status = xir_compile_resource_status(xr_compile_session_poll(request->session));
        if (status != XR_XIR_OK) goto done;
        bool same=false;status=lsp_path_equal(s->texts[i].path,s->texts[j].path,&same);
        if(status!=XR_XIR_OK)goto done;
        if(!same)continue;
        if(s->texts[i].version!=s->texts[j].version||s->texts[i].length!=s->texts[j].length){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=lsp_equal(s->texts[i].text,s->texts[j].text,s->texts[i].length,&same);
        if(status!=XR_XIR_OK)goto done;
        if(!same){status=XR_XIR_BAD_STRUCTURE;goto done;}
    }
    {
        XrXirSourceRequest selected = *request; selected.entry_path = s->texts[entry].path;
        status = xr_xir_compile_source_check_overlay(&selected, inputs, count, &result, diagnostic, failure);
        if (status != XR_XIR_OK) goto done;
        const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
        if (!view || !view->complete) { status = XR_XIR_BAD_STRUCTURE; goto done; }
        for(size_t i=0;i<count;++i) {
            LspSourceText *d=&s->texts[i];
            if(view->module_count) {
                status=xir_compile_resource_status(xr_compile_resources_calloc(s->resources,view->module_count,sizeof(*d->modules),(void **)&d->modules));
                if(status!=XR_XIR_OK)goto done;
            }
            for(uint32_t m=0;m<view->module_count;++m) {
                status = xir_compile_resource_status(xr_compile_session_poll(request->session));
                if (status != XR_XIR_OK) goto done;
                bool equal=false;status=lsp_path_equal(d->path,view->modules[m].path,&equal);
                if(status!=XR_XIR_OK)goto done;
                if(!equal)continue;
                d->modules[d->module_count++]=m;d->module=m;
            }
        }
        s->query = result.snapshot; result.snapshot = NULL;
    }
    status = xir_compile_resource_status(xr_compile_session_poll(request->session));
    if (status != XR_XIR_OK) goto done;
    *output = s; s = NULL;
done:
    xr_xir_compile_source_result_free(&result); xr_compile_resources_free(inputs);
    xlsp_source_snapshot_free(s); return status;
}
XrXirStatus xlsp_source_snapshot_matches(const XlspSourceSnapshot *s,
    const XlspSourceDocument *documents, size_t count, size_t entry, bool *matches) {
    if (!s || !matches || (!documents && count)) return XR_XIR_BAD_STRUCTURE;
    bool equal = count == s->count && entry == s->entry;
    for (size_t i = 0; equal && i < count; ++i) {
        const LspSourceText *a = &s->texts[i]; const XlspSourceDocument *b = &documents[i];
        XrXirStatus status;
        if (!b->uri || !b->text) return XR_XIR_BAD_STRUCTURE;
        equal = a->version == b->version && a->uri_length == b->uri_length && a->length == b->length;
        if (equal) { status = lsp_equal(a->uri, b->uri, a->uri_length, &equal); if (status != XR_XIR_OK) return status; }
        if (equal) { status = lsp_equal(a->text, b->text, a->length, &equal); if (status != XR_XIR_OK) return status; }
    }
    *matches = equal; return XR_XIR_OK;
}
static XrXirStatus lsp_target(const XrXirSourceView *view, uint32_t *id) {
    for (uint32_t n = 0; n < view->declaration_count; ++n) {
        if (!*id || *id > view->declaration_count) return XR_XIR_UNRESOLVED;
        const XrXirSourceDeclaration *d = &view->declarations[*id - 1];
        if (!d->target || d->target == *id) return XR_XIR_OK;
        *id = d->target;
    }
    return XR_XIR_BAD_STRUCTURE;
}
static bool lsp_contains(XrXirSourceRange r, uint32_t module, int line, int column) {
    return r.module == module && (line > r.line || (line == r.line && column >= r.column)) &&
        (line < r.end_line || (line == r.end_line && column < r.end_column));
}
static XrXirStatus lsp_location(const XlspSourceSnapshot *,XrXirSourceRange,const char *,XlspSourceLocation *);
static XrXirStatus lsp_symbol_equal(const XrXirSourceView *view,
    uint32_t a_id,uint32_t b_id,bool *equal) {
    if(a_id==b_id){*equal=true;return XR_XIR_OK;}
    XrXirSourceRange a=view->declarations[a_id-1].range,b=view->declarations[b_id-1].range;
    if(a.module>=view->module_count||b.module>=view->module_count)return XR_XIR_BAD_STRUCTURE;
    bool same=a.module==b.module;
    XrXirStatus status=XR_XIR_OK;
    if(!same)status=lsp_path_equal(view->modules[a.module].path,view->modules[b.module].path,&same);
    if(status!=XR_XIR_OK)return status;
    *equal=same&&a.line==b.line&&a.column==b.column&&a.end_line==b.end_line&&a.end_column==b.end_column;
    return XR_XIR_OK;
}
static XrXirStatus lsp_choose_symbol(const XrXirSourceView *view,
    uint32_t candidate,uint32_t *chosen) {
    XrXirStatus status=lsp_target(view,&candidate);if(status!=XR_XIR_OK)return status;
    if(!*chosen){*chosen=candidate;return XR_XIR_OK;}
    bool equal=false;status=lsp_symbol_equal(view,*chosen,candidate,&equal);
    return status!=XR_XIR_OK?status:equal?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static bool lsp_absent_selection(XrXirSourceRange range) {
    return !range.line&&!range.column&&!range.end_line&&!range.end_column;
}
static const XrXirSourceSyntaxView *lsp_syntax(const XlspSourceSnapshot *s,const XrXirSourceView *view) {
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(s->query);
    return syntax&&syntax->declaration_count==view->declaration_count&&syntax->reference_count==view->reference_count?syntax:NULL;
}
/* Authenticate a producer-selected name against owned bytes. Identity was
 * already resolved by Source; this does not perform name-based resolution. */
static XrXirStatus lsp_name_selection(const LspSourceText *text,
    XrXirSourceRange range,const char *name) {
    if(!name||range.line<=0||range.column<=0||range.end_line!=range.line||range.end_column<=range.column)return XR_XIR_UNSUPPORTED;
    size_t length=0;
    for(;;) {
        if(!name[length])break;
        if(length==SIZE_MAX-1)return XR_XIR_BUDGET;++length;
    }
    if(length!=(size_t)(range.end_column-range.column))return XR_XIR_UNSUPPORTED;
    XrLspPosition ignored={0};size_t begin=0,end=0;int line=range.line,column=range.column;
    XrXirStatus status=lsp_position_at(text,false,&ignored,&line,&column,&begin);if(status!=XR_XIR_OK)return status;
    line=range.end_line;column=range.end_column;
    status=lsp_position_at(text,false,&ignored,&line,&column,&end);if(status!=XR_XIR_OK)return status;
    if(end<begin||end-begin!=length)return XR_XIR_UNSUPPORTED;
    bool equal=false;status=lsp_equal(text->text+begin,name,length,&equal);
    return status!=XR_XIR_OK?status:equal?XR_XIR_OK:XR_XIR_UNSUPPORTED;
}
static XrXirStatus lsp_symbol(const XlspSourceSnapshot *s, const char *uri, size_t length,
    XrLspPosition position, uint32_t *id) {
    if (!s || !uri) return XR_XIR_BAD_STRUCTURE;
    const LspSourceText *d = NULL;
    for (size_t i = 0; i < s->count; ++i) {
        if (s->texts[i].uri_length != length) continue;
        bool equal = false; XrXirStatus status = lsp_equal(s->texts[i].uri, uri, length, &equal);
        if (status != XR_XIR_OK) return status;
        if (equal) { d = &s->texts[i]; break; }
    }
    if (!d || d->module == UINT32_MAX) return XR_XIR_UNRESOLVED;
    int line = 0, column = 0; XrXirStatus status = lsp_position(d, true, &position, &line, &column);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=lsp_syntax(s,view);if(!syntax)return XR_XIR_UNRESOLVED;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *r = &view->references[i];
        XrXirSourceRange selection=syntax->references[i];if(lsp_absent_selection(selection))continue;
        bool matches=false;status=lsp_text_has_module(d,r->range.module,&matches);
        if(status!=XR_XIR_OK)return status;
        if(matches&&lsp_contains(selection,selection.module,line,column)) {
            status=lsp_name_selection(d,selection,view->declarations[r->declaration-1].name);if(status!=XR_XIR_OK)return status;
            status=lsp_choose_symbol(view,r->target?r->target:r->declaration,id);
            if(status!=XR_XIR_OK)return status;
        }
    }
    if(*id)return XR_XIR_OK;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *declaration = &view->declarations[i];
        XrXirSourceRange selection=syntax->declarations[i].name;if(lsp_absent_selection(selection))continue;
        bool matches=false;status=lsp_text_has_module(d,declaration->range.module,&matches);
        if(status!=XR_XIR_OK)return status;
        if(matches&&lsp_contains(selection,selection.module,line,column)) {
            if(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)return XR_XIR_UNSUPPORTED;
            status=lsp_name_selection(d,selection,declaration->name);if(status!=XR_XIR_OK)return status;
            status=lsp_choose_symbol(view,declaration->id,id);if(status!=XR_XIR_OK)return status;
        }
    }
    return *id?XR_XIR_OK:XR_XIR_UNRESOLVED;
}
static XrXirStatus lsp_symbol_text_equal(const char *a,const char *b,bool *equal) {
    if(!a||!b)return XR_XIR_BAD_STRUCTURE;
    for(;;++a,++b) {
        if(*a!=*b){*equal=false;return XR_XIR_OK;}
        if(!*a){*equal=true;return XR_XIR_OK;}
    }
}
static XrXirStatus lsp_symbol_version(const XlspSourceSnapshot *s,uint32_t module,bool *known,int64_t *version) {
    bool found=false;int64_t selected=0;
    for(size_t i=0;i<s->count;++i) {
        XrXirStatus status;
        bool matches=false;status=lsp_text_has_module(&s->texts[i],module,&matches);if(status!=XR_XIR_OK)return status;
        if(!matches)continue;
        if(found&&selected!=s->texts[i].version)return XR_XIR_BAD_STRUCTURE;
        found=true;selected=s->texts[i].version;
    }
    *known=found;*version=selected;return XR_XIR_OK;
}
XrXirStatus xlsp_source_same_symbol(const XlspSourceSnapshot *left,const XlspSourceSnapshot *right,
    const char *uri,size_t length,XrLspPosition position,bool *equal) {
    if(!left||!right||!uri||length==SIZE_MAX||!equal||left->resources!=right->resources)return XR_XIR_BAD_STRUCTURE;
    uint32_t a_id=0,b_id=0;XrXirStatus status=lsp_symbol(left,uri,length,position,&a_id);
    if(status!=XR_XIR_OK)return status;
    status=lsp_symbol(right,uri,length,position,&b_id);if(status!=XR_XIR_OK)return status;
    const XrXirSourceView *a_view=xr_xir_compile_source_snapshot_view(left->query);
    const XrXirSourceView *b_view=xr_xir_compile_source_snapshot_view(right->query);
    const XrXirSourceDeclaration *a=&a_view->declarations[a_id-1],*b=&b_view->declarations[b_id-1];
    if(a->range.module>=a_view->module_count||b->range.module>=b_view->module_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceQueryModule *a_module=&a_view->modules[a->range.module],*b_module=&b_view->modules[b->range.module];
    bool same=a->kind==b->kind&&a->native_identity==b->native_identity&&a->range.line==b->range.line&&
        a->range.column==b->range.column&&a->range.end_line==b->range.end_line&&a->range.end_column==b->range.end_column&&
        !memcmp(a_module->fingerprint.bytes,b_module->fingerprint.bytes,sizeof(a_module->fingerprint.bytes));
    if(same){status=lsp_symbol_text_equal(a_module->identity,b_module->identity,&same);if(status!=XR_XIR_OK)return status;}
    if(same){status=lsp_symbol_text_equal(a->name,b->name,&same);if(status!=XR_XIR_OK)return status;}
    if(same) {
        bool a_known=false,b_known=false;int64_t a_version=0,b_version=0;
        status=lsp_symbol_version(left,a->range.module,&a_known,&a_version);if(status!=XR_XIR_OK)return status;
        status=lsp_symbol_version(right,b->range.module,&b_known,&b_version);if(status!=XR_XIR_OK)return status;
        same=a_known==b_known&&(!a_known||a_version==b_version);
    }
    *equal=same;return XR_XIR_OK;
}
static XrXirStatus lsp_location(const XlspSourceSnapshot *s, XrXirSourceRange r,const char *name, XlspSourceLocation *out) {
    for (size_t i = 0; i < s->count; ++i) {
        XrXirStatus status;
        const LspSourceText *d = &s->texts[i];bool matches=false;
        status=lsp_text_has_module(d,r.module,&matches);if(status!=XR_XIR_OK)return status;
        if(!matches)continue;
        status=lsp_name_selection(d,r,name);if(status!=XR_XIR_OK)return status;
        XlspSourceLocation location = {d->uri, {{0,0},{0,0}}, d->version, XR_XIR_SOURCE_READ};
        status = lsp_position(d, false, &location.range.start, &r.line, &r.column);
        if (status != XR_XIR_OK) return status;
        status = lsp_position(d, false, &location.range.end, &r.end_line, &r.end_column);
        if (status == XR_XIR_OK) *out = location;
        return status;
    }
    return XR_XIR_UNRESOLVED;
}
XR_FUNC XrXirStatus xlsp_source_declaration_kind(const XlspSourceSnapshot *s,const char *uri,
    size_t length,XrLspPosition position,XrXirSourceDeclarationKind *out) {
    if(!s||!uri||length==SIZE_MAX||!out)return XR_XIR_BAD_STRUCTURE;
    const LspSourceText *text=NULL;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<s->count;++i) {
        if(s->texts[i].uri_length!=length)continue;
        bool same=false;status=lsp_equal(s->texts[i].uri,uri,length,&same);if(status!=XR_XIR_OK)return status;
        if(same){text=&s->texts[i];break;}
    }
    if(!text||text->module==UINT32_MAX)return XR_XIR_UNRESOLVED;
    int line=0,column=0;status=lsp_position(text,true,&position,&line,&column);if(status!=XR_XIR_OK)return status;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=lsp_syntax(s,view);if(!syntax)return XR_XIR_UNRESOLVED;
    bool found=false;XrXirSourceDeclarationKind kind=XR_XIR_SOURCE_FUNCTION;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *decl=&view->declarations[i];
        const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[i];
        if(lsp_absent_selection(fact->name))continue;
        bool matches=false;status=lsp_text_has_module(text,decl->range.module,&matches);
        if(status!=XR_XIR_OK)return status;
        if(!matches||!lsp_contains(fact->name,fact->name.module,line,column))continue;
        if((uint32_t)decl->kind>XR_XIR_SOURCE_INTRINSIC)return XR_XIR_BAD_STRUCTURE;
        if(fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)return XR_XIR_UNSUPPORTED;
        status=lsp_name_selection(text,fact->name,decl->name);if(status!=XR_XIR_OK)return status;
        if(found&&kind!=decl->kind)return XR_XIR_BAD_STRUCTURE;
        kind=decl->kind;found=true;
    }
    if(!found)return XR_XIR_UNRESOLVED;*out=kind;return XR_XIR_OK;
}
XrXirStatus xlsp_source_definition(const XlspSourceSnapshot *s, const char *uri,
    size_t length, XrLspPosition position, XlspSourceLocation *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t id = 0; XrXirStatus status = lsp_symbol(s, uri, length, position, &id);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=lsp_syntax(s,view);
    if(!syntax||lsp_absent_selection(syntax->declarations[id-1].name))return XR_XIR_UNRESOLVED;
    return lsp_location(s, syntax->declarations[id-1].name,view->declarations[id-1].name, output);
}
void xlsp_source_locations_free(XlspSourceLocations *locations) {
    if (!locations) return;
    xr_compile_resources_free(locations->items); *locations = (XlspSourceLocations){0};
}
XrXirStatus xlsp_source_references(const XlspSourceSnapshot *s, const char *uri,
    size_t length, XrLspPosition position, bool include_declaration, XlspSourceLocations *output) {
    if (!output || output->items || output->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t id = 0; XrXirStatus status = lsp_symbol(s, uri, length, position, &id);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=lsp_syntax(s,view);if(!syntax)return XR_XIR_UNRESOLVED;
    bool declaration_visible=include_declaration&&!lsp_absent_selection(syntax->declarations[id-1].name);
    size_t count = declaration_visible ? 1 : 0;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        if(view->references[i].access==XR_XIR_SOURCE_IMPORT_SELECTOR)continue;
        if(lsp_absent_selection(syntax->references[i]))continue;
        uint32_t target = view->references[i].target ? view->references[i].target : view->references[i].declaration;
        status = lsp_target(view, &target); if (status != XR_XIR_OK) return status;
        bool same=false;status=lsp_symbol_equal(view,id,target,&same);
        if(status!=XR_XIR_OK)return status;
        if(same) { if(count==SIZE_MAX)return XR_XIR_BUDGET;++count; }
    }
    if (count > SIZE_MAX / sizeof(XlspSourceLocation)) return XR_XIR_BUDGET;
    XlspSourceLocations locations = {0};
    if (count) {
        status = xir_compile_resource_status(xr_compile_resources_alloc(s->resources,
            count * sizeof(*locations.items), (void **)&locations.items));
        if (status != XR_XIR_OK) return status;
    }
    if (declaration_visible) {
        status = lsp_location(s, syntax->declarations[id-1].name,view->declarations[id-1].name, &locations.items[locations.count]);
        if (status != XR_XIR_OK) goto done;
        locations.items[locations.count].access = XR_XIR_SOURCE_WRITE;
        ++locations.count;
    }
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        if(view->references[i].access==XR_XIR_SOURCE_IMPORT_SELECTOR)continue;
        if(lsp_absent_selection(syntax->references[i]))continue;
        const XrXirSourceReference *r = &view->references[i]; uint32_t target = r->target ? r->target : r->declaration;
        status = lsp_target(view, &target); if (status != XR_XIR_OK) goto done;
        bool same=false;status=lsp_symbol_equal(view,id,target,&same);
        if(status!=XR_XIR_OK)goto done;
        if(!same)continue;
        status = lsp_location(s, syntax->references[i],view->declarations[r->declaration-1].name, &locations.items[locations.count]);
        if (status != XR_XIR_OK) goto done;
        locations.items[locations.count].access = r->access;
        ++locations.count;
    }
    *output = locations; return XR_XIR_OK;
done:
    xlsp_source_locations_free(&locations); return status;
}

XrXirStatus xlsp_source_snapshot_build(const XrXirSourceRequest *request,
    const XlspSourceDocument *documents,size_t count,size_t entry,
    XlspSourceSnapshot **output,XrXirSourceDiagnostic *diagnostic,XrXirSourceFailure *failure) {
    return xlsp_source_snapshot_build_authorities(request,documents,NULL,count,entry,output,diagnostic,failure);
}
XrXirStatus xlsp_source_uri_path(XrCompileResources *resources,const char *uri,size_t length,char **out) {
    if(!resources||!uri||length==SIZE_MAX||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceSnapshot s={0};s.resources=resources;
    LspSourceText d={0};d.uri=(char *)uri;d.uri_length=length;
    XrXirStatus status=lsp_uri_path(&s,&d);
    if(status==XR_XIR_OK)*out=d.path;else xr_compile_resources_free(d.path);
    return status;
}
XrXirStatus xlsp_source_snapshot_contains(const XlspSourceSnapshot *s,const char *uri,size_t length,bool *out) {
    if(!s||!uri||!out)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<s->count;++i) {
        XrXirStatus status;
        if(s->texts[i].uri_length!=length)continue;
        bool equal=false;status=lsp_equal(s->texts[i].uri,uri,length,&equal);
        if(status!=XR_XIR_OK)return status;
        if(equal){*out=s->texts[i].module!=UINT32_MAX;return XR_XIR_OK;}
    }
    *out=false;return XR_XIR_OK;
}

XrXirStatus xlsp_source_position(XrCompileResources *resources,const char *text,size_t length,
    int line,int column,XrLspPosition *out) {
    if(!resources||!text||length==SIZE_MAX||!out)return XR_XIR_BAD_STRUCTURE;
    LspSourceText d={0};d.text=(char *)text;d.length=length;
    XrLspPosition position={0};XrXirStatus status=lsp_position(&d,false,&position,&line,&column);
    if(status==XR_XIR_OK)*out=position;
    return status;
}

XR_FUNC XrXirStatus xlsp_source_cursor(XrCompileResources *resources,const char *text,size_t length,
    XrLspPosition position,XlspSourceCursor *out) {
    if(!resources||!text||length==SIZE_MAX||!out)return XR_XIR_BAD_STRUCTURE;
    LspSourceText d={0};d.text=(char *)text;d.length=length;
    XlspSourceCursor cursor={0};
    XrXirStatus status=lsp_position_at(&d,true,&position,&cursor.line,&cursor.column,&cursor.offset);
    if(status==XR_XIR_OK)*out=cursor;
    return status;
}

#include "xlsp_source_hover_impl.h"
#include "xlsp_source_signature_query.inc.c"

#include "xlsp_source_semantic_impl.h"
#include "xlsp_source_type_identity.inc.c"
#include "xlsp_source_inlay_query.inc.c"
#include "xlsp_source_parameter_hints.inc.c"
