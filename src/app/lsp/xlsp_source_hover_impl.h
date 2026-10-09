/* Included only by the modern immutable Source/query implementation. */
#include "../../xir/xxir_nominal.h"
#include "../../xir/xxir_types.h"
#include "../../shared/xr_param_mode.h"

typedef struct LspHoverBuilder {
    const XlspSourceSnapshot *snapshot;
    char *text;
    size_t length, capacity;
    XrXirStatus status;
} LspHoverBuilder;
static void hover_bytes(LspHoverBuilder *b,const char *text,size_t length) {
    if(b->status!=XR_XIR_OK)return;
    if(!text||length>SIZE_MAX-b->length-1){b->status=XR_XIR_BAD_STRUCTURE;return;}
    size_t needed=b->length+length+1;
    if(needed>b->capacity) {
        size_t next=b->capacity?b->capacity:64;
        while(next<needed) {
            b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)return;
            if(next>SIZE_MAX/2){next=needed;break;}next*=2;
        }
        b->status=lsp_resource_status(xr_compile_resources_resize(b->snapshot->resources,(void **)&b->text,next));
        if(b->status!=XR_XIR_OK)return;b->capacity=next;
    }
    b->status=lsp_work(b->snapshot,length+1);
    if(b->status==XR_XIR_OK){memcpy(b->text+b->length,text,length);b->length+=length;b->text[b->length]=0;}
}
static void hover_text(LspHoverBuilder *b,const char *text) {
    if(b->status!=XR_XIR_OK)return;
    if(!text){b->status=XR_XIR_BAD_STRUCTURE;return;}
    size_t length=0;
    for(;;) {
        b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)return;
        if(!text[length])break;
        if(length==SIZE_MAX-1){b->status=XR_XIR_BUDGET;return;}++length;
    }
    hover_bytes(b,text,length);
}
static const char *hover_builtin(XrXirType type) {
    switch(type) {
    case XR_XIR_UNIT:return "Unit";case XR_XIR_BOOL:return "bool";
    case XR_XIR_I8:return "i8";case XR_XIR_I16:return "i16";case XR_XIR_I32:return "i32";case XR_XIR_I64:return "i64";
    case XR_XIR_U8:return "u8";case XR_XIR_U16:return "u16";case XR_XIR_U32:return "u32";case XR_XIR_U64:return "u64";
    case XR_XIR_F32:return "f32";case XR_XIR_F64:return "f64";case XR_XIR_STRING:return "String";
    case XR_XIR_ERROR:return "Error";case XR_XIR_PANIC_INFO:return "PanicInfo";case XR_XIR_RUNE:return "Rune";
    default:return NULL;
    }
}
static const XrXirTypeNode *hover_node(const XrXirSourceView *v,XrXirType type) {
    uint32_t raw=(uint32_t)type;
    return v->types&&raw>=XR_XIR_CONSTRUCTED_TYPE_BASE&&raw<XR_XIR_TYPE_PARAMETER_BASE&&
        raw-XR_XIR_CONSTRUCTED_TYPE_BASE<v->types->count?v->types->nodes+raw-XR_XIR_CONSTRUCTED_TYPE_BASE:NULL;
}
static void hover_mode(LspHoverBuilder *b,uint32_t mode) {
    if(mode==XR_PARAM_REF)hover_text(b,"ref ");
    else if(mode==XR_PARAM_MOVE)hover_text(b,"move ");
    else if(mode!=XR_PARAM_READ)b->status=XR_XIR_BAD_TYPE;
}
typedef struct LspHoverTypeFrame {XrXirType type;uint32_t stage,index;} LspHoverTypeFrame;
/* Explicit traversal stack: no native recursion and no guessed depth bound.
 * A verified type graph is acyclic; an active path cannot exceed pool+1. */
static void hover_type(LspHoverBuilder *b,const XrXirSourceView *v,XrXirSourceType source) {
    if(b->status!=XR_XIR_OK)return;
    if(!source.known){b->status=XR_XIR_UNRESOLVED;return;}
    LspHoverTypeFrame *stack=NULL;size_t count=0,capacity=0;
    XrXirType next=source.type;bool push=true;
    while((push||count)&&b->status==XR_XIR_OK) {
        b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)break;
        if(push) {
            if(v->types&&count>(size_t)v->types->count){b->status=XR_XIR_BAD_TYPE;break;}
            if(count==capacity) {
                size_t cap=capacity?capacity*2:8;
                if(cap<capacity||cap>SIZE_MAX/sizeof(*stack)){b->status=XR_XIR_BUDGET;break;}
                b->status=lsp_resource_status(xr_compile_resources_resize(b->snapshot->resources,(void **)&stack,cap*sizeof(*stack)));
                if(b->status!=XR_XIR_OK)break;capacity=cap;
            }
            b->status=lsp_work(b->snapshot,sizeof(*stack));if(b->status!=XR_XIR_OK)break;
            stack[count++]=(LspHoverTypeFrame){next,0,0};push=false;
        }
        LspHoverTypeFrame *f=&stack[count-1];
        const char *builtin=hover_builtin(f->type);
        if(builtin){hover_text(b,builtin);--count;continue;}
        if((uint32_t)f->type>=XR_XIR_TYPE_PARAMETER_BASE){b->status=XR_XIR_UNSUPPORTED;break;}
        const XrXirTypeNode *n=hover_node(v,f->type);
        if(!n){b->status=XR_XIR_BAD_TYPE;break;}
        if(n->kind==XR_XIR_TYPE_ARRAY||n->kind==XR_XIR_TYPE_CELL||n->kind==XR_XIR_TYPE_ATOMIC||n->kind==XR_XIR_TYPE_TASK||n->kind==XR_XIR_TYPE_NULLABLE) {
            if(!f->stage) {
                hover_text(b,n->kind==XR_XIR_TYPE_ARRAY?"Array<":n->kind==XR_XIR_TYPE_CELL?"Cell<":
                    n->kind==XR_XIR_TYPE_ATOMIC?"Atomic<":n->kind==XR_XIR_TYPE_TASK?"Task<":"Nullable<");
                f->stage=1;next=n->element;push=true;
            } else {hover_text(b,">");--count;}
            continue;
        }
        if(n->kind==XR_XIR_TYPE_NOMINAL) {
            const XrXirNominalTable *table=v->types->nominals;
            if(!table||!table->declarations||n->nominal.declaration>=table->count){b->status=XR_XIR_BAD_TYPE;break;}
            const XrXirNominalDeclaration *d=&table->declarations[n->nominal.declaration];
            if(!f->stage){hover_bytes(b,(const char *)d->name.bytes,d->name.length);f->stage=1;if(n->nominal.argument_count)hover_text(b,"<");}
            if(f->index<n->nominal.argument_count) {
                if(!n->nominal.arguments){b->status=XR_XIR_BAD_TYPE;break;}
                if(f->index)hover_text(b,", ");next=n->nominal.arguments[f->index++];push=true;
            } else {if(n->nominal.argument_count)hover_text(b,">");--count;}
            continue;
        }
        if(n->kind==XR_XIR_TYPE_CALLABLE||n->kind==XR_XIR_TYPE_TUPLE) {
            if(!f->stage){hover_text(b,n->kind==XR_XIR_TYPE_CALLABLE?"fn(":"(");f->stage=1;}
            if(f->stage==1&&f->index<n->parameter_count) {
                if(!n->parameters){b->status=XR_XIR_BAD_TYPE;break;}
                const XrXirCallableParameter *p=&n->parameters[f->index];
                if(f->index++)hover_text(b,", ");
                if(n->kind==XR_XIR_TYPE_CALLABLE&&!xr_xir_callable_parameter_storage_valid(v->types,p)) {
                    b->status=XR_XIR_BAD_TYPE;break;
                }
                hover_mode(b,p->mode);next=p->type;
                if(n->kind==XR_XIR_TYPE_CALLABLE&&p->mode==XR_PARAM_REF)
                    next=xr_xir_cell_element(v->types,p->type);
                push=true;continue;
            }
            if(f->stage==1) {
                if(n->kind==XR_XIR_TYPE_TUPLE){if(n->parameter_count==1)hover_text(b,",");hover_text(b,")");--count;continue;}
                hover_text(b,") -> ");f->stage=2;next=n->result;push=true;continue;
            }
            if(n->flags&XR_XIR_CALLABLE_NO_SUSPEND)hover_text(b," [no_suspend]");
            switch(n->flags&XR_XIR_CALLABLE_ROOT_MASK) {
            case XR_XIR_CALLABLE_ROOT_NONE:hover_text(b," [root: NONE]");break;
            case XR_XIR_CALLABLE_ROOT_REQUIRED:hover_text(b," [root: REQUIRED]");break;
            case XR_XIR_CALLABLE_ROOT_UNRESOLVED:hover_text(b," [root: UNRESOLVED]");break;
            default:b->status=XR_XIR_BAD_TYPE;break;
            }
            --count;continue;
        }
        b->status=XR_XIR_UNSUPPORTED;
    }
    xr_compile_resources_free(stack);
}
static void hover_qualified_name(LspHoverBuilder *b,const XrXirSourceView *v,const XrXirSourceDeclaration *d) {
    /* A member's actual immediate owner supplies the qualifier. Do not invent
     * namespace paths, overload indices or missing generic parameter names. */
    if(d->parent) {
        if(d->parent>v->declaration_count){b->status=XR_XIR_BAD_STRUCTURE;return;}
        const XrXirSourceDeclaration *parent=&v->declarations[d->parent-1];
        if(parent->kind==XR_XIR_SOURCE_TYPE){hover_text(b,parent->name);hover_text(b,".");}
    }
    hover_text(b,d->name);
}
static void hover_declaration(LspHoverBuilder *b,const XrXirSourceView *v,const XrXirSourceDeclaration *d) {
    /* Governed native signature is already an owned Source fact. */
    if(d->signature&&*d->signature){hover_text(b,d->signature);return;}
    if(d->generic_parameter_count||d->generic_parent_count){b->status=XR_XIR_UNSUPPORTED;return;}
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(b->snapshot->query);
    if(!syntax||syntax->declaration_count!=v->declaration_count||!d->id||d->id>syntax->declaration_count){b->status=XR_XIR_UNRESOLVED;return;}
    const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[d->id-1];
    if(fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED){b->status=XR_XIR_UNSUPPORTED;return;}
    if(fact->role==XR_XIR_SOURCE_SYNTAX_METHOD&&d->kind!=XR_XIR_SOURCE_FUNCTION){b->status=XR_XIR_UNSUPPORTED;return;}
    if(d->exported)hover_text(b,"export ");
    if(fact->role==XR_XIR_SOURCE_SYNTAX_METHOD) {
        if(fact->flags&XR_XIR_SOURCE_SYNTAX_STATIC)hover_text(b,"static ");
        for(uint32_t i=0;i<syntax->marker_count&&b->status==XR_XIR_OK;++i) {
            b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)break;
            const XrXirSourceMarker *marker=&syntax->markers[i];
            if(marker->declaration!=d->id||marker->role==XR_XIR_SOURCE_MARKER_REF_ARGUMENT)continue;
            hover_mode(b,marker->role==XR_XIR_SOURCE_MARKER_REF?XR_PARAM_REF:XR_PARAM_MOVE);
        }
    }
    if(d->kind==XR_XIR_SOURCE_FUNCTION) {
        if(fact->role==XR_XIR_SOURCE_SYNTAX_CLOSURE)hover_text(b,"fn");
        else {hover_text(b,"fn ");hover_qualified_name(b,v,d);}
        hover_text(b,"(");uint32_t index=0,shown=0;
        for(uint32_t i=0;i<v->declaration_count&&b->status==XR_XIR_OK;++i) {
            b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)break;
            const XrXirSourceDeclaration *p=&v->declarations[i];
            if(p->kind!=XR_XIR_SOURCE_PARAMETER||p->parent!=d->id)continue;
            if(index>=d->parameter_count||!d->parameters||!p->type.known||!d->parameters[index].known||
                p->type.type!=d->parameters[index].type||p->type.generic_owner!=d->parameters[index].generic_owner) {
                b->status=XR_XIR_BAD_STRUCTURE;break;
            }
            ++index;
            if(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_RECEIVER)continue;
            if(shown++)hover_text(b,", ");if(p->mutable)hover_text(b,"ref ");
            hover_text(b,p->name);hover_text(b,": ");hover_type(b,v,p->type);
        }
        if(b->status==XR_XIR_OK&&index!=d->parameter_count)b->status=XR_XIR_UNSUPPORTED;
        hover_text(b,") -> ");hover_type(b,v,d->type);return;
    }
    if(d->kind==XR_XIR_SOURCE_TYPE) {
        const XrXirTypeNode *n=d->type.known?hover_node(v,d->type.type):NULL;
        const XrXirNominalDeclaration *nominal=NULL;
        if(n&&n->kind==XR_XIR_TYPE_NOMINAL&&v->types->nominals&&v->types->nominals->declarations&&n->nominal.declaration<v->types->nominals->count)
            nominal=&v->types->nominals->declarations[n->nominal.declaration];
        bool own=false;
        if(nominal) {
            size_t length=0;for(;;) {
                b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)return;
                if(!d->name[length])break;++length;
            }
            if(length==nominal->name.length){b->status=lsp_equal(b->snapshot,d->name,nominal->name.bytes,length,&own);if(b->status!=XR_XIR_OK)return;}
            if(own) {
                if(d->range.module>=v->module_count){b->status=XR_XIR_BAD_STRUCTURE;return;}
                const char *identity=v->modules[d->range.module].identity;length=0;
                if(!identity){b->status=XR_XIR_BAD_STRUCTURE;return;}
                for(;;){b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)return;if(!identity[length])break;++length;}
                own=false;
                if(length==nominal->module.length){b->status=lsp_equal(b->snapshot,identity,nominal->module.bytes,length,&own);if(b->status!=XR_XIR_OK)return;}
            }
        }
        if(own) {
            if(nominal->kind>XR_XIR_NOMINAL_CLASS){b->status=XR_XIR_BAD_TYPE;return;}
            if(nominal->flags&XR_XIR_NOMINAL_FINAL)hover_text(b,"final ");
            hover_text(b,nominal->kind==XR_XIR_NOMINAL_CLASS?"class ":nominal->kind==XR_XIR_NOMINAL_ENUM?"enum ":"struct ");
            hover_text(b,d->name);return;
        }
        hover_text(b,"type ");hover_text(b,d->name);hover_text(b," = ");hover_type(b,v,d->type);return;
    }
    if(d->kind==XR_XIR_SOURCE_BINDING||d->kind==XR_XIR_SOURCE_MEMBER||d->kind==XR_XIR_SOURCE_PARAMETER) {
        hover_text(b,d->kind==XR_XIR_SOURCE_PARAMETER?(d->mutable?"ref ":"parameter "):d->mutable?"var ":"const ");
        hover_qualified_name(b,v,d);hover_text(b,": ");hover_type(b,v,d->type);return;
    }
    b->status=XR_XIR_UNSUPPORTED;
}
XrXirStatus xlsp_source_hover(const XlspSourceSnapshot *s,const char *uri,size_t length,
    XrLspPosition position,XlspSourceHover *output) {
    if(!s||!uri||length==SIZE_MAX||!output||output->text||output->length)return XR_XIR_BAD_STRUCTURE;
    uint32_t id=0;XrXirStatus status=lsp_symbol(s,uri,length,position,&id);if(status!=XR_XIR_OK)return status;
    const XrXirSourceView *v=xr_xir_compile_source_snapshot_view(s->query);
    LspHoverBuilder builder={s,NULL,0,0,XR_XIR_OK};
    hover_declaration(&builder,v,&v->declarations[id-1]);
    if(builder.status==XR_XIR_OK){*output=(XlspSourceHover){builder.text,builder.length};return XR_XIR_OK;}
    xr_compile_resources_free(builder.text);return builder.status;
}
void xlsp_source_hover_free(XlspSourceHover *hover) {
    if(hover){xr_compile_resources_free(hover->text);*hover=(XlspSourceHover){0};}
}
