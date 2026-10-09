/* Immutable Checked query facts; no analyzer/scope-name rediscovery. */
#include "../../frontend/lexer/xlex.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../xir/xxir_interface.h"
struct XlspSourceTokens {XrCompileResources *resources;XlspSourceToken *items;size_t count,capacity;};
void xlsp_source_tokens_free(XlspSourceTokens *tokens) {
    if(tokens){xr_compile_resources_free(tokens->items);xr_compile_resources_free(tokens);}
}
const XlspSourceToken *xlsp_source_tokens_items(const XlspSourceTokens *tokens,size_t *count) {
    if(count)*count=tokens?tokens->count:0;return tokens?tokens->items:NULL;
}
static XrXirStatus semantic_append(const XlspSourceSnapshot *s,XlspSourceTokens *out,XlspSourceToken token) {
    if(!token.length||token.column>INT32_MAX||token.line>INT32_MAX||token.length>INT32_MAX-token.column||token.type>=22)return XR_XIR_BAD_STRUCTURE;
    if(out->count==out->capacity) {
        size_t next=out->capacity?out->capacity*2:32;
        if(next<out->capacity||next>SIZE_MAX/sizeof(*out->items))return XR_XIR_BUDGET;
        XrXirStatus status=lsp_resource_status(xr_compile_resources_resize(s->resources,(void **)&out->items,next*sizeof(*out->items)));
        if(status!=XR_XIR_OK)return status;out->capacity=next;
    }
    XrXirStatus status=lsp_work(s,sizeof(token));if(status==XR_XIR_OK)out->items[out->count++]=token;return status;
}
static XrXirStatus semantic_literal_equal(const XlspSourceSnapshot *s,const char *text,XrXirLiteral literal,bool *same) {
    *same=false;if(!text||(!literal.bytes&&literal.length))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<literal.length;++i) {
        XrXirStatus status=lsp_work(s,2);if(status!=XR_XIR_OK)return status;
        if(!text[i]||text[i]!=literal.bytes[i])return XR_XIR_OK;
    }
    XrXirStatus status=lsp_work(s,1);if(status==XR_XIR_OK)*same=text[literal.length]==0;return status;
}
static XrXirStatus semantic_interface(const XlspSourceSnapshot *s,const XrXirSourceView *v,const XrXirSourceDeclaration *d,bool *matched) {
    *matched=false;if(!v->types||!v->types->interfaces)return XR_XIR_OK;
    if(d->range.module>=v->module_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceTable *table=v->types->interfaces;
    for(uint32_t i=0;i<table->count;++i) {
        bool same=false;XrXirStatus status=semantic_literal_equal(s,d->name,table->declarations[i].name,&same);
        if(status!=XR_XIR_OK)return status;if(!same)continue;
        status=semantic_literal_equal(s,v->modules[d->range.module].identity,table->declarations[i].module,&same);
        if(status!=XR_XIR_OK)return status;if(same){*matched=true;return XR_XIR_OK;}
    }
    return XR_XIR_OK;
}
static XrXirStatus semantic_kind(const XlspSourceSnapshot *s,const XrXirSourceView *v,
    const XrXirSourceDeclaration *d,uint32_t *kind,uint32_t *modifiers) {
    *modifiers=d->native_identity?256u:0u;
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(s->query);
    if(!syntax||!d->id||d->id>syntax->declaration_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[d->id-1];
    if(fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)return XR_XIR_UNSUPPORTED;
    if(fact->role==XR_XIR_SOURCE_SYNTAX_METHOD) {
        *kind=13;if(fact->flags&XR_XIR_SOURCE_SYNTAX_STATIC)*modifiers|=8;
        return XR_XIR_OK;
    }
    switch(d->kind) {
    case XR_XIR_SOURCE_FUNCTION:
        /* Nested functions require the copied producer syntax role. */
        if(d->parent>v->declaration_count)return XR_XIR_BAD_STRUCTURE;
        if(d->parent&&fact->role!=XR_XIR_SOURCE_SYNTAX_CLOSURE)return XR_XIR_UNSUPPORTED;
        *kind=12;return XR_XIR_OK;
    case XR_XIR_SOURCE_BINDING:*kind=8;if(!d->mutable)*modifiers|=4;return XR_XIR_OK;
    case XR_XIR_SOURCE_PARAMETER:
        *kind=7;if(!d->mutable)*modifiers|=4;return XR_XIR_OK;
    case XR_XIR_SOURCE_TYPE_PARAMETER:*kind=6;return XR_XIR_OK;
    case XR_XIR_SOURCE_MODULE:*kind=0;return XR_XIR_OK;
    case XR_XIR_SOURCE_TYPE: {
        const XrXirTypeNode *n=d->type.known?hover_node(v,d->type.type):NULL;
        if(n&&n->kind==XR_XIR_TYPE_NOMINAL&&v->types->nominals&&v->types->nominals->declarations&&n->nominal.declaration<v->types->nominals->count) {
            uint32_t k=v->types->nominals->declarations[n->nominal.declaration].kind;
            if(k>XR_XIR_NOMINAL_CLASS)return XR_XIR_BAD_TYPE;
            *kind=k==XR_XIR_NOMINAL_ENUM?3:k==XR_XIR_NOMINAL_CLASS?2:5;return XR_XIR_OK;
        }
        if(d->type.known||d->native_identity){*kind=1;return XR_XIR_OK;}
        bool matched_interface=false;XrXirStatus status=semantic_interface(s,v,d,&matched_interface);
        if(status!=XR_XIR_OK)return status;if(matched_interface){*kind=4;return XR_XIR_OK;}return XR_XIR_UNSUPPORTED;
    }
    case XR_XIR_SOURCE_MEMBER: {
        if(!d->parent||d->parent>v->declaration_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclaration *parent=&v->declarations[d->parent-1];
        const XrXirTypeNode *n=parent->type.known?hover_node(v,parent->type.type):NULL;
        if(parent->kind==XR_XIR_SOURCE_TYPE) {
            if(!n||n->kind!=XR_XIR_TYPE_NOMINAL||!v->types->nominals||!v->types->nominals->declarations||n->nominal.declaration>=v->types->nominals->count) {
                bool matched_interface=false;XrXirStatus status=semantic_interface(s,v,parent,&matched_interface);
                if(status!=XR_XIR_OK)return status;if(matched_interface){*kind=13;return XR_XIR_OK;}return XR_XIR_UNSUPPORTED;
            }
            *kind=v->types->nominals->declarations[n->nominal.declaration].kind==XR_XIR_NOMINAL_ENUM?10:9;
        } else if(parent->kind==XR_XIR_SOURCE_MEMBER)*kind=9;
        else return XR_XIR_UNSUPPORTED;
        if(!d->mutable)*modifiers|=4;return XR_XIR_OK;
    }
    case XR_XIR_SOURCE_INTRINSIC:
        if(d->signature&&*d->signature){*kind=d->parent?13:12;return XR_XIR_OK;}
        if(d->type.known){*kind=d->parent?9:12;if(d->parent)*modifiers|=4;return XR_XIR_OK;}
        return XR_XIR_UNSUPPORTED;
    default:break;
    }
    (void)s;return XR_XIR_UNSUPPORTED;
}
static XrXirStatus semantic_range(const XlspSourceSnapshot *s,const LspSourceText *text,
    XrXirSourceRange range,uint32_t kind,uint32_t modifiers,XlspSourceTokens *out) {
    int line=range.line,column=range.column;XrLspPosition start={0},end={0};
    XrXirStatus status=lsp_position(s,text,false,&start,&line,&column);if(status!=XR_XIR_OK)return status;
    line=range.end_line;column=range.end_column;status=lsp_position(s,text,false,&end,&line,&column);if(status!=XR_XIR_OK)return status;
    if(start.line!=end.line||end.character<=start.character)return XR_XIR_UNSUPPORTED;
    return semantic_append(s,out,(XlspSourceToken){start.line,start.character,end.character-start.character,kind,modifiers});
}
static int semantic_compare(XlspSourceToken a,XlspSourceToken b) {
    if(a.line!=b.line)return a.line<b.line?-1:1;
    if(a.column!=b.column)return a.column<b.column?-1:1;
    if(a.length!=b.length)return a.length<b.length?-1:1;return 0;
}
static XrXirStatus semantic_sort(const XlspSourceSnapshot *s,XlspSourceTokens *tokens) {
    if(tokens->count<2)return XR_XIR_OK;
    XlspSourceToken *scratch=NULL;XrXirStatus status=lsp_resource_status(xr_compile_resources_alloc(s->resources,tokens->count*sizeof(*scratch),(void **)&scratch));
    if(status!=XR_XIR_OK)return status;
    XlspSourceToken *input=tokens->items,*output=scratch;
    for(size_t width=1;width<tokens->count&&status==XR_XIR_OK;) {
        for(size_t begin=0;begin<tokens->count;) {
            size_t middle=begin+(tokens->count-begin<width?tokens->count-begin:width);
            size_t end=middle+(tokens->count-middle<width?tokens->count-middle:width),left=begin,right=middle;
            for(size_t index=begin;index<end;++index) {
                status=lsp_work(s,sizeof(*scratch)+5);if(status!=XR_XIR_OK)break;
                output[index]=right==end||(left<middle&&semantic_compare(input[left],input[right])<=0)?input[left++]:input[right++];
            }
            if(status!=XR_XIR_OK)break;begin=end;
        }
        XlspSourceToken *swap=input;input=output;output=swap;
        if(width>tokens->count/2)break;width*=2;
    }
    if(status==XR_XIR_OK&&input!=tokens->items) {
        status=lsp_work(s,tokens->count*sizeof(*scratch));if(status==XR_XIR_OK)memcpy(tokens->items,input,tokens->count*sizeof(*scratch));
    }
    xr_compile_resources_free(scratch);if(status!=XR_XIR_OK)return status;
    size_t kept=0;
    for(size_t i=0;i<tokens->count;++i) {
        status=lsp_work(s,sizeof(*scratch)+5);if(status!=XR_XIR_OK)return status;
        XlspSourceToken t=tokens->items[i];
        if(kept&&semantic_compare(tokens->items[kept-1],t)==0) {
            if(tokens->items[kept-1].type!=t.type)return XR_XIR_BAD_STRUCTURE;
            tokens->items[kept-1].modifiers|=t.modifiers;continue;
        }
        if(kept&&tokens->items[kept-1].line==t.line&&tokens->items[kept-1].column+tokens->items[kept-1].length>t.column)return XR_XIR_BAD_STRUCTURE;
        tokens->items[kept++]=t;
    }
    tokens->count=kept;return XR_XIR_OK;
}
static XrXirStatus semantic_lexical_spans(const XlspSourceSnapshot *s,const LspSourceText *text,const XlspSourceTokens *out) {
    XrCompilerSession *session=NULL;XrArena arena={0};XrXirStatus status=XR_XIR_OK;
    size_t matched=0;
    XrCompilerSessionStatus created=xr_compile_session_new(s->resources,&session);
    if(created!=XR_COMPILER_SESSION_OK)return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrCompileState *state=xr_compile_session_compile_state(session);XrArenaBacking backing;
    if(xr_compiler_arena_state_backing(state,&backing)!=XR_ARENA_OK){status=XR_XIR_BAD_STRUCTURE;goto done;}
    (void)xr_arena_open(&arena,XR_ARENA_SEGMENT_SIZE,&backing);
    status=lsp_resource_status(xr_compiler_arena_capture_status(&arena,state));if(status!=XR_XIR_OK)goto done;
    Scanner scanner={0};status=lsp_resource_status(xr_compile_scanner_open(&scanner,state,&arena,text->text));
    while(status==XR_XIR_OK) {
        Token token=xr_scanner_scan(&scanner);status=lsp_resource_status(xr_compile_state_status(state));if(status!=XR_XIR_OK)break;
        if(token.type==TK_EOF){if(matched!=out->count)status=XR_XIR_UNSUPPORTED;break;}if(token.type==TK_ERROR){status=XR_XIR_BAD_STRUCTURE;break;}
        if(token.type==TK_TEMPLATE_STRING||token.type==TK_RAW_TEMPLATE_STRING){status=XR_XIR_UNSUPPORTED;break;}
        if(token.length<=0||token.column>INT_MAX-token.length){status=XR_XIR_BAD_STRUCTURE;break;}
        int line=token.line,column=token.column;XrLspPosition start={0};bool semantic=false;
        status=lsp_position(s,text,false,&start,&line,&column);if(status!=XR_XIR_OK)break;
        if(matched<out->count) {
            const XlspSourceToken *expected=&out->items[matched];
            if(expected->line<start.line||(expected->line==start.line&&expected->column<start.character)){status=XR_XIR_UNSUPPORTED;break;}
            if(expected->line==start.line&&expected->column==start.character) {
                /* A Source expression range is not necessarily one name.
                 * Never color a whole a.b/call/closure as a single symbol. */
                bool name=token.type==TK_NAME||token.type==TK_THIS||token.type==TK_FROM||token.type==TK_TO||
                    token.type==TK_DEFAULT||token.type==TK_CANCELLED||token.type==TK_REF||token.type==TK_MOVE||
                    (token.type>=TK_FIRST_KEYWORD&&token.type<=TK_LAST_KEYWORD)||
                    (token.type==TK_LITERAL_INT&&expected->type==9);
                if(expected->type==16) {
                    /* The producer role already proved this is a modifier;
                     * this check only authenticates its exact lexical bytes. */
                    if(lsp_work(s,4)!=XR_XIR_OK){status=XR_XIR_BUDGET;break;}
                    bool proved=(token.length==3&&!memcmp(token.start,"ref",3))||
                        (token.length==4&&!memcmp(token.start,"move",4));
                    if(!proved){status=XR_XIR_BAD_STRUCTURE;break;}
                } else if(!name){status=XR_XIR_UNSUPPORTED;break;}
                XrLspPosition end={0};line=token.line;column=token.column+token.length;
                status=lsp_position(s,text,false,&end,&line,&column);if(status!=XR_XIR_OK)break;
                if(end.line!=start.line||end.character-start.character!=expected->length){status=XR_XIR_UNSUPPORTED;break;}
                ++matched;semantic=true;
            }
        }
        /* These are contextual tokens: spelling alone cannot prove a ref or
         * move operation. Their exact owned syntax roles are a successor. */
        if(!semantic&&(token.type==TK_REF||token.type==TK_MOVE)){status=XR_XIR_UNSUPPORTED;break;}
    }
done:
    xr_arena_destroy(&arena);xr_compile_session_free(session);return status;
}
XrXirStatus xlsp_source_semantic_tokens(const XlspSourceSnapshot *s,const char *uri,size_t length,XlspSourceTokens **output) {
    if(!s||!uri||length==SIZE_MAX||!output||*output)return XR_XIR_BAD_STRUCTURE;
    const LspSourceText *text=NULL;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<s->count;++i) {
        status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;if(s->texts[i].uri_length!=length)continue;
        bool equal=false;status=lsp_equal(s,s->texts[i].uri,uri,length,&equal);if(status!=XR_XIR_OK)return status;if(equal){text=&s->texts[i];break;}
    }
    if(!text||text->module==UINT32_MAX)return XR_XIR_UNRESOLVED;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);XlspSourceTokens *tokens=NULL;
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(s->query);
    if(!syntax||syntax->declaration_count!=view->declaration_count||syntax->reference_count!=view->reference_count)return XR_XIR_UNRESOLVED;
    status=lsp_resource_status(xr_compile_resources_calloc(s->resources,1,sizeof(*tokens),(void **)&tokens));if(status!=XR_XIR_OK)return status;
    tokens->resources=s->resources;
    for(uint32_t i=0;i<view->declaration_count&&status==XR_XIR_OK;++i) {
        const XrXirSourceDeclaration *decl=&view->declarations[i];bool matches=false;
        status=lsp_text_has_module(s,text,decl->range.module,&matches);if(status!=XR_XIR_OK)break;if(!matches)continue;
        const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[i];
        if(fact->role==XR_XIR_SOURCE_SYNTAX_NONE||fact->role==XR_XIR_SOURCE_SYNTAX_CLOSURE||fact->role==XR_XIR_SOURCE_SYNTAX_RECEIVER)continue;
        if(fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED){status=XR_XIR_UNSUPPORTED;break;}
        uint32_t target=decl->id;status=lsp_target(s,view,&target);if(status!=XR_XIR_OK)break;
        uint32_t kind=0,mods=0;status=semantic_kind(s,view,&view->declarations[target-1],&kind,&mods);if(status!=XR_XIR_OK)break;
        mods|=1;if(kind==12||kind==13||kind==2||kind==3||kind==4||kind==5)mods|=2;
        status=semantic_range(s,text,fact->name,kind,mods,tokens);
    }
    for(uint32_t i=0;i<view->reference_count&&status==XR_XIR_OK;++i) {
        const XrXirSourceReference *ref=&view->references[i];bool matches=false;
        status=lsp_text_has_module(s,text,ref->range.module,&matches);if(status!=XR_XIR_OK)break;if(!matches)continue;
        XrXirSourceRange selection=syntax->references[i];
        if(!selection.line&&!selection.column&&!selection.end_line&&!selection.end_column)continue;
        uint32_t target=ref->target?ref->target:ref->declaration;status=lsp_target(s,view,&target);if(status!=XR_XIR_OK)break;
        uint32_t kind=0,mods=0;status=semantic_kind(s,view,&view->declarations[target-1],&kind,&mods);if(status!=XR_XIR_OK)break;
        if(view->declarations[target-1].kind==XR_XIR_SOURCE_INTRINSIC&&
            (ref->access==XR_XIR_SOURCE_CALL||ref->access==XR_XIR_SOURCE_FUNCTION_VALUE)) {
            kind=view->declarations[target-1].parent?13:12;mods&=~4u;
        }
        if(ref->access==XR_XIR_SOURCE_WRITE||ref->access==XR_XIR_SOURCE_READ_WRITE)mods|=64;
        status=semantic_range(s,text,selection,kind,mods,tokens);
    }
    for(uint32_t i=0;i<syntax->marker_count&&status==XR_XIR_OK;++i) {
        const XrXirSourceMarker *marker=&syntax->markers[i];bool matches=false;
        status=lsp_text_has_module(s,text,marker->range.module,&matches);if(status!=XR_XIR_OK)break;if(!matches)continue;
        status=semantic_range(s,text,marker->range,16,0,tokens);
    }
    if(status==XR_XIR_OK)status=semantic_sort(s,tokens);
    if(status==XR_XIR_OK)status=semantic_lexical_spans(s,text,tokens);
    if(status==XR_XIR_OK){*output=tokens;tokens=NULL;}xlsp_source_tokens_free(tokens);return status;
}
XrXirStatus xlsp_source_semantic_range(const XlspSourceSnapshot *s,const char *uri,size_t length,XrLspRange range) {
    if(!s||!uri||length==SIZE_MAX||range.start.line>range.end.line||
        (range.start.line==range.end.line&&range.start.character>range.end.character))return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<s->count;++i) {
        XrXirStatus status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;if(s->texts[i].uri_length!=length)continue;
        bool equal=false;status=lsp_equal(s,s->texts[i].uri,uri,length,&equal);if(status!=XR_XIR_OK)return status;if(!equal)continue;
        int line=0,column=0;status=lsp_position(s,&s->texts[i],true,&range.start,&line,&column);
        if(status==XR_XIR_OK)status=lsp_position(s,&s->texts[i],true,&range.end,&line,&column);return status;
    }
    return XR_XIR_UNRESOLVED;
}
