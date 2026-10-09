/* Only the modern syntax owner includes this implementation. */
struct XlspSourceOutline {
    XrCompileResources *resources;
    XlspSourceSymbol *items;
    size_t count,capacity;
};
typedef struct OutlineFrame {const AstNode *node;uint32_t parent;} OutlineFrame;
void xlsp_source_outline_free(XlspSourceOutline *outline) {
    if(!outline)return;
    for(size_t i=0;i<outline->count;++i)xr_compile_resources_free((char *)outline->items[i].name);
    xr_compile_resources_free(outline->items);xr_compile_resources_free(outline);
}
const XlspSourceSymbol *xlsp_source_outline_symbols(const XlspSourceOutline *outline,size_t *count) {
    if(count)*count=outline?outline->count:0;return outline?outline->items:NULL;
}
static bool outline_position_le(XrLspPosition a,XrLspPosition b) {
    return a.line<b.line||(a.line==b.line&&a.character<=b.character);
}
static XrXirStatus outline_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus outline_append(const XlspSyntaxSnapshot *s,XlspSourceOutline *o,
    const AstNode *node,const char *name,uint32_t kind,uint32_t parent,uint32_t *index) {
    if(!name||!kind||!node||node->line<=0||node->column<=0||node->end_line<=0||node->end_column<=0)return XR_XIR_UNRESOLVED;
    size_t length=0;XrXirStatus status=XR_XIR_OK;
    for(;;) {
        status=syntax_work(s,1);if(status!=XR_XIR_OK)return status;
        if(!name[length])break;if(length==SIZE_MAX-1)return XR_XIR_BUDGET;++length;
    }
    if(!length||length>(size_t)(INT_MAX-node->column))return XR_XIR_BAD_STRUCTURE;
    XlspSourceSymbol symbol={0};symbol.kind=kind;symbol.parent=parent;
    status=xlsp_source_position(o->resources,s->text,s->length,node->line,node->column,&symbol.range.start);
    if(status==XR_XIR_OK)status=xlsp_source_position(o->resources,s->text,s->length,node->end_line,node->end_column,&symbol.range.end);
    symbol.selection.start=symbol.range.start;
    if(status==XR_XIR_OK)status=xlsp_source_position(o->resources,s->text,s->length,node->line,node->column+(int)length,&symbol.selection.end);
    if(status!=XR_XIR_OK)return status;
    if(!outline_position_le(symbol.selection.end,symbol.range.end)||
        !outline_position_le(symbol.range.start,symbol.selection.end))return XR_XIR_BAD_STRUCTURE;
    if(parent!=UINT32_MAX) {
        if(parent>=o->count)return XR_XIR_BAD_STRUCTURE;
        const XrLspRange *p=&o->items[parent].range;
        if(!outline_position_le(p->start,symbol.range.start)||!outline_position_le(symbol.range.end,p->end))return XR_XIR_BAD_STRUCTURE;
    }
    if(o->count>=UINT32_MAX)return XR_XIR_BUDGET;
    if(o->count==o->capacity) {
        size_t capacity=o->capacity?o->capacity*2:16;
        if(capacity<o->capacity||capacity>SIZE_MAX/sizeof(*o->items))return XR_XIR_BUDGET;
        status=outline_resource(xr_compile_resources_resize(o->resources,(void **)&o->items,capacity*sizeof(*o->items)));
        if(status!=XR_XIR_OK)return status;o->capacity=capacity;
    }
    char *copy=NULL;status=outline_resource(xr_compile_resources_alloc(o->resources,length+1,(void **)&copy));
    if(status!=XR_XIR_OK)return status;
    status=syntax_work(s,length+1+sizeof(symbol));
    if(status!=XR_XIR_OK){xr_compile_resources_free(copy);return status;}
    memcpy(copy,name,length+1);symbol.name=copy;*index=(uint32_t)o->count;o->items[o->count++]=symbol;return XR_XIR_OK;
}
static XrXirStatus outline_push(const XlspSyntaxSnapshot *s,OutlineFrame **stack,size_t *count,size_t *capacity,const AstNode *node,uint32_t parent) {
    if(!node)return XR_XIR_BAD_STRUCTURE;
    if(*count==*capacity) {
        size_t next=*capacity?*capacity*2:16;
        if(next<*capacity||next>SIZE_MAX/sizeof(**stack))return XR_XIR_BUDGET;
        XrXirStatus status=outline_resource(xr_compile_state_resize(s->state,(void **)stack,next*sizeof(**stack)));
        if(status!=XR_XIR_OK)return status;*capacity=next;
    }
    XrXirStatus status=syntax_work(s,sizeof(**stack));
    if(status==XR_XIR_OK)(*stack)[(*count)++]=(OutlineFrame){node,parent};return status;
}
XrXirStatus xlsp_source_syntax_outline(const XlspSyntaxSnapshot *s,XlspSourceOutline **output) {
    if(!s||!s->ast||!output||*output)return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources=xr_compile_state_resources(s->state);XlspSourceOutline *o=NULL;
    XrXirStatus status=outline_resource(xr_compile_resources_calloc(resources,1,sizeof(*o),(void **)&o));
    if(status!=XR_XIR_OK)return status;o->resources=resources;
    OutlineFrame *stack=NULL;size_t count=0,capacity=0;
    status=outline_push(s,&stack,&count,&capacity,s->ast,UINT32_MAX);
    while(count&&status==XR_XIR_OK) {
        status=syntax_work(s,sizeof(AstNode));if(status!=XR_XIR_OK)break;
        OutlineFrame frame=stack[--count];const AstNode *node=frame.node;const char *name=NULL;uint32_t kind=0,index=UINT32_MAX;
        switch(node->type) {
        case AST_FUNCTION_DECL:name=node->as.function_decl.name;kind=LSP_SYMBOL_FUNCTION;break;
        case AST_VAR_DECL:case AST_CONST_DECL:name=node->as.var_decl.name;kind=node->type==AST_CONST_DECL?LSP_SYMBOL_CONSTANT:LSP_SYMBOL_VARIABLE;break;
        case AST_CLASS_DECL:case AST_STRUCT_DECL:case AST_UNION_DECL:name=node->as.class_decl.name;kind=LSP_SYMBOL_CLASS;break;
        case AST_FIELD_DECL:name=node->as.field_decl.name;kind=LSP_SYMBOL_FIELD;break;
        case AST_METHOD_DECL:name=node->as.method_decl.name;kind=node->as.method_decl.is_constructor?LSP_SYMBOL_CONSTRUCTOR:LSP_SYMBOL_METHOD;break;
        case AST_INTERFACE_DECL:name=node->as.interface_decl.name;kind=LSP_SYMBOL_INTERFACE;break;
        case AST_ENUM_DECL:name=node->as.enum_decl.name;kind=LSP_SYMBOL_ENUM;break;
        case AST_ENUM_MEMBER:name=node->as.enum_member.name;kind=LSP_SYMBOL_ENUM_MEMBER;break;
        case AST_TYPE_ALIAS:status=XR_XIR_UNSUPPORTED;break;
        default:break;
        }
        if(kind)status=outline_append(s,o,node,name,kind,frame.parent,&index);
#define OUTLINE_PUSH(n,p) do {if(status==XR_XIR_OK)status=outline_push(s,&stack,&count,&capacity,(n),(p));} while(0)
        if(status!=XR_XIR_OK)break;
        if(node->type==AST_PROGRAM) {
            if(node->as.program.count<0||(node->as.program.count&&!node->as.program.statements)){status=XR_XIR_BAD_STRUCTURE;break;}
            for(int i=node->as.program.count;i>0&&status==XR_XIR_OK;--i)OUTLINE_PUSH(node->as.program.statements[i-1],UINT32_MAX);
        } else if(node->type==AST_CLASS_DECL||node->type==AST_STRUCT_DECL||node->type==AST_UNION_DECL) {
            const ClassDeclNode *c=&node->as.class_decl;
            if(c->method_count<0||c->field_count<0||(c->method_count&&!c->methods)||(c->field_count&&!c->fields)){status=XR_XIR_BAD_STRUCTURE;break;}
            for(int i=c->method_count;i>0&&status==XR_XIR_OK;--i) {
                if(!c->methods[i-1]||c->methods[i-1]->type!=AST_METHOD_DECL){status=XR_XIR_BAD_STRUCTURE;break;}
                OUTLINE_PUSH(c->methods[i-1],index);
            }
            for(int i=c->field_count;i>0&&status==XR_XIR_OK;--i) {
                if(!c->fields[i-1]||c->fields[i-1]->type!=AST_FIELD_DECL){status=XR_XIR_BAD_STRUCTURE;break;}
                OUTLINE_PUSH(c->fields[i-1],index);
            }
        } else if(node->type==AST_ENUM_DECL) {
            const EnumDeclNode *e=&node->as.enum_decl;
            if(e->member_count<0||(e->member_count&&!e->members)){status=XR_XIR_BAD_STRUCTURE;break;}
            for(int i=e->member_count;i>0&&status==XR_XIR_OK;--i) {
                if(!e->members[i-1]||e->members[i-1]->type!=AST_ENUM_MEMBER){status=XR_XIR_BAD_STRUCTURE;break;}
                OUTLINE_PUSH(e->members[i-1],index);
            }
        }
#undef OUTLINE_PUSH
    }
    xr_compile_state_free(stack);
    if(status==XR_XIR_OK){*output=o;o=NULL;}xlsp_source_outline_free(o);return status;
}
static XrJsonValue *outline_position_json(XrLspPosition p) {
    XrJsonValue *v=xjson_new_object();if(!v)return NULL;
    if(!syntax_member(v,"line",xjson_new_number(p.line))||!syntax_member(v,"character",xjson_new_number(p.character))){xjson_free(v);return NULL;}return v;
}
static XrJsonValue *outline_range_json(XrLspRange r) {
    XrJsonValue *v=xjson_new_object();if(!v)return NULL;
    if(!syntax_member(v,"start",outline_position_json(r.start))||!syntax_member(v,"end",outline_position_json(r.end))){xjson_free(v);return NULL;}return v;
}
XrXirStatus xlsp_source_outline_json(const XlspSourceOutline *o,XrJsonValue **output) {
    if(!o||!output||*output)return XR_XIR_BAD_STRUCTURE;
    XrJsonValue *result=xjson_new_array();if(!result)return XR_XIR_OUT_OF_MEMORY;
    XrJsonValue **children=NULL;XrXirStatus status=XR_XIR_OK;
    if(o->count) {
        status=outline_resource(xr_compile_resources_calloc(o->resources,o->count,sizeof(*children),(void **)&children));
        if(status!=XR_XIR_OK)goto done;
    }
    for(size_t i=0;i<o->count;++i) {
        status=outline_resource(xr_compile_resources_work(o->resources,sizeof(*o->items)));if(status!=XR_XIR_OK)break;
        const XlspSourceSymbol *s=&o->items[i];
        if(s->parent!=UINT32_MAX&&s->parent>=i){status=XR_XIR_BAD_STRUCTURE;break;}
        XrJsonValue *array=s->parent==UINT32_MAX?result:children[s->parent];
        if(!array){status=XR_XIR_BAD_STRUCTURE;break;}
        XrJsonValue *symbol=xjson_new_object();if(!symbol){status=XR_XIR_OUT_OF_MEMORY;break;}
        if(!syntax_member(symbol,"name",xjson_new_string(s->name))||!syntax_member(symbol,"kind",xjson_new_number(s->kind))||
            !syntax_member(symbol,"range",outline_range_json(s->range))||!syntax_member(symbol,"selectionRange",outline_range_json(s->selection))) {
            xjson_free(symbol);status=XR_XIR_OUT_OF_MEMORY;break;
        }
        /* Child relationship is known before publication; empty children are
         * omitted. Records are pre-order with each parent's direct children
         * contiguous in this language's outline contract. */
        if(i+1<o->count&&o->items[i+1].parent==i) {
            children[i]=xjson_new_array();
            if(!children[i]||!syntax_member(symbol,"children",children[i])){children[i]=NULL;xjson_free(symbol);status=XR_XIR_OUT_OF_MEMORY;break;}
        }
        if(array->as.array.count==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){xjson_free(symbol);status=XR_XIR_BUDGET;break;}
        int before=array->as.array.count;xjson_array_push(array,symbol);
        if(array->as.array.count!=before+1){xjson_free(symbol);status=XR_XIR_OUT_OF_MEMORY;break;}
    }
done:
    xr_compile_resources_free(children);
    if(status==XR_XIR_OK){*output=result;result=NULL;}xjson_free(result);return status;
}
