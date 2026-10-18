/* Copyright (c) 2026 Xinglei Xu. MIT License. */
/* Finite display identity from verified pool facts; no type inference or authority. */
static void type_identity_number(LspHoverBuilder *b,uint64_t value) {
    uint8_t bytes[8];for(unsigned i=0;i<8;++i)bytes[i]=(uint8_t)(value>>(i*8));hover_bytes(b,(const char *)bytes,sizeof(bytes));
}
static void type_identity_bytes(LspHoverBuilder *b,const void *bytes,size_t count) {
    if(!bytes&&count){b->status=XR_XIR_BAD_STRUCTURE;return;}
    type_identity_number(b,count);if(count)hover_bytes(b,(const char *)bytes,count);
}
static void type_identity_text(LspHoverBuilder *b,const char *text) {
    if(!text){type_identity_number(b,UINT64_MAX);return;}
    size_t length=0;
    while(b->status==XR_XIR_OK) {
        b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)return;
        if(!text[length])break;if(length==SIZE_MAX-1){b->status=XR_XIR_BUDGET;return;}++length;
    }
    type_identity_bytes(b,text,length);
}
static void type_identity_module(LspHoverBuilder *b,const XrXirSourceView *view,uint32_t module) {
    if(module>=view->module_count){b->status=XR_XIR_BAD_STRUCTURE;return;}
    const XrXirSourceQueryModule *source=&view->modules[module];
    type_identity_text(b,source->identity);type_identity_text(b,source->path);
    type_identity_bytes(b,&source->fingerprint,sizeof(source->fingerprint));
    const XlspSourceSnapshot *snapshot=b->snapshot;const LspSourceText *selected=NULL;
    for(size_t i=0;i<snapshot->count&&b->status==XR_XIR_OK;++i) {
        bool contains=false;b->status=lsp_text_has_module(snapshot,&snapshot->texts[i],module,&contains);
        if(contains){selected=&snapshot->texts[i];break;}
    }
    type_identity_number(b,selected?1:0);
    if(selected){type_identity_text(b,selected->uri);type_identity_number(b,(uint64_t)selected->version);}
}
static void type_identity_declaration(LspHoverBuilder *b,const XrXirSourceView *view,uint32_t id) {
    const XrXirSourceSyntaxView *syntax=lsp_syntax(b->snapshot,view);
    if(!id||id>view->declaration_count||!syntax||id>syntax->declaration_count){b->status=XR_XIR_UNRESOLVED;return;}
    const XrXirSourceDeclaration *declaration=&view->declarations[id-1];
    const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[id-1];
    type_identity_module(b,view,declaration->range.module);type_identity_text(b,declaration->name);
    type_identity_number(b,declaration->kind);type_identity_number(b,declaration->native_identity);
    type_identity_number(b,fact->role);type_identity_number(b,(uint64_t)fact->name.line);
    type_identity_number(b,(uint64_t)fact->name.column);type_identity_number(b,(uint64_t)fact->name.end_line);
    type_identity_number(b,(uint64_t)fact->name.end_column);type_identity_number(b,fact->type_parameter_count);
    for(uint32_t i=0;i<fact->type_parameter_count&&b->status==XR_XIR_OK;++i) {
        if(!fact->type_parameters){b->status=XR_XIR_BAD_STRUCTURE;break;}
        type_identity_text(b,fact->type_parameters[i]);
    }
}
static void type_identity_nominal(LspHoverBuilder *b,const XrXirSourceView *view,const XrXirTypeNode *node) {
    const XrXirNominalTable *table=view->types->nominals;
    if(!table||!table->declarations||node->nominal.declaration>=table->count){b->status=XR_XIR_BAD_TYPE;return;}
    const XrXirNominalDeclaration *declaration=&table->declarations[node->nominal.declaration];
    type_identity_bytes(b,declaration->module.bytes,declaration->module.length);
    type_identity_bytes(b,declaration->name.bytes,declaration->name.length);
    type_identity_number(b,declaration->kind);type_identity_number(b,declaration->flags);
    type_identity_number(b,declaration->exported);type_identity_number(b,declaration->parameter_count);
    type_identity_number(b,declaration->field_count);type_identity_number(b,declaration->variant_count);
    type_identity_number(b,declaration->native.native_id);
    type_identity_bytes(b,declaration->native.source_fingerprint,sizeof(declaration->native.source_fingerprint));
    bool found=false;
    for(uint32_t i=0;i<view->module_count&&b->status==XR_XIR_OK;++i) {
        const char *name=view->modules[i].identity;size_t length=0;
        if(!name){b->status=XR_XIR_BAD_STRUCTURE;break;}
        while(b->status==XR_XIR_OK) {
            b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK||!name[length])break;++length;
        }
        if(length!=declaration->module.length)continue;
        b->status=lsp_work(b->snapshot,length);if(b->status!=XR_XIR_OK)break;
        if(memcmp(name,declaration->module.bytes,length))continue;
        type_identity_module(b,view,i);found=true;break;
    }
    if(!found&&!declaration->native.native_id&&b->status==XR_XIR_OK)b->status=XR_XIR_UNRESOLVED;
    type_identity_number(b,found?1:0);
    for(uint32_t i=0;i<declaration->field_count&&b->status==XR_XIR_OK;++i) {
        if(!declaration->fields){b->status=XR_XIR_BAD_STRUCTURE;break;}
        type_identity_bytes(b,declaration->fields[i].name.bytes,declaration->fields[i].name.length);
        type_identity_number(b,declaration->fields[i].flags);
    }
    for(uint32_t i=0;i<declaration->variant_count&&b->status==XR_XIR_OK;++i) {
        if(!declaration->variants){b->status=XR_XIR_BAD_STRUCTURE;break;}
        const XrXirNominalVariant *variant=&declaration->variants[i];
        type_identity_bytes(b,variant->name.bytes,variant->name.length);
        type_identity_number(b,variant->field_begin);type_identity_number(b,variant->field_count);
    }
}
typedef struct TypeIdentityFrame {XrXirType type;uint64_t index;bool started;} TypeIdentityFrame;
static bool type_identity_child(const XrXirTypeNode *node,uint64_t index,XrXirType *type,uint32_t *mode) {
    *mode=0;
    switch(node->kind) {
    case XR_XIR_TYPE_ARRAY:case XR_XIR_TYPE_CELL:case XR_XIR_TYPE_NULLABLE:case XR_XIR_TYPE_ATOMIC:case XR_XIR_TYPE_TASK:
        if(index)return false;*type=node->element;return true;
    case XR_XIR_TYPE_CALLABLE:case XR_XIR_TYPE_TUPLE:
        if(index<node->parameter_count){*type=node->parameters[index].type;*mode=node->parameters[index].mode;return true;}
        if(index==node->parameter_count&&node->kind==XR_XIR_TYPE_CALLABLE){*type=node->result;return true;}return false;
    case XR_XIR_TYPE_NOMINAL:
        if(index<node->nominal.argument_count){*type=node->nominal.arguments[index];return true;}
        index-=node->nominal.argument_count;
        if(index<node->nominal.field_count){*type=node->nominal.fields[index];return true;}return false;
    default:return false;
    }
}
static void type_identity_graph(LspHoverBuilder *b,const XrXirSourceView *view,XrXirSourceType source) {
    TypeIdentityFrame *stack=NULL;size_t count=0,capacity=0;XrXirType next=source.type;bool push=true;
    while((push||count)&&b->status==XR_XIR_OK) {
        b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)break;
        if(push) {
            bool active=false;
            for(size_t i=0;i<count;++i) {
                b->status=lsp_work(b->snapshot,1);if(b->status!=XR_XIR_OK)break;
                if(stack[i].type==next){type_identity_number(b,4);type_identity_number(b,count-i);active=true;break;}
            }
            push=false;if(active||b->status!=XR_XIR_OK)continue;
            if(view->types&&count>(size_t)view->types->count){b->status=XR_XIR_BAD_TYPE;break;}
            if(count==capacity) {
                size_t cap=capacity?capacity*2:8;
                if(cap<capacity||cap>SIZE_MAX/sizeof(*stack)){b->status=XR_XIR_BUDGET;break;}
                b->status=lsp_resource_status(xr_compile_resources_resize(b->snapshot->resources,(void **)&stack,cap*sizeof(*stack)));
                if(b->status!=XR_XIR_OK)break;capacity=cap;
            }
            b->status=lsp_work(b->snapshot,sizeof(*stack));if(b->status!=XR_XIR_OK)break;
            stack[count++]=(TypeIdentityFrame){next,0,false};
        }
        TypeIdentityFrame *frame=&stack[count-1];uint32_t raw=(uint32_t)frame->type;
        if(hover_builtin(frame->type)){type_identity_number(b,0);type_identity_number(b,raw);--count;continue;}
        if(raw>=XR_XIR_TYPE_PARAMETER_BASE&&raw<XR_XIR_TYPE_PARAMETER_LIMIT) {
            if(!source.generic_owner){b->status=XR_XIR_UNRESOLVED;break;}
            type_identity_number(b,1);type_identity_number(b,raw-XR_XIR_TYPE_PARAMETER_BASE);--count;continue;
        }
        const XrXirTypeNode *node=hover_node(view,frame->type);if(!node){b->status=XR_XIR_BAD_TYPE;break;}
        if(!frame->started) {
            if(node->kind<XR_XIR_TYPE_CALLABLE||node->kind>XR_XIR_TYPE_TASK){b->status=XR_XIR_UNSUPPORTED;break;}
            type_identity_number(b,2);type_identity_number(b,node->kind);type_identity_number(b,node->flags);
            type_identity_number(b,node->parameter_span);type_identity_number(b,node->parameter_count);
            type_identity_number(b,node->nominal.argument_count);type_identity_number(b,node->nominal.field_count);
            if(node->kind==XR_XIR_TYPE_NOMINAL)type_identity_nominal(b,view,node);frame->started=true;
        }
        XrXirType child=XR_XIR_UNIT;uint32_t mode=0;
        if(type_identity_child(node,frame->index,&child,&mode)) {
            ++frame->index;type_identity_number(b,mode);next=child;push=true;
        } else {type_identity_number(b,3);--count;}
    }
    xr_compile_resources_free(stack);
}
static void type_hint_identity(LspHoverBuilder *b,const XrXirSourceView *view,const XrXirSourceDeclaration *declaration) {
    if(!declaration->type.known){b->status=XR_XIR_UNRESOLVED;return;}
    type_identity_number(b,1);type_identity_declaration(b,view,declaration->id);
    type_identity_number(b,declaration->type.generic_owner?1:0);
    if(declaration->type.generic_owner)type_identity_declaration(b,view,declaration->type.generic_owner);
    type_identity_graph(b,view,declaration->type);
}
