/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_markers.inc.c - Current type derivations for Error permission
 *
 * KEY CONCEPT:
 *   Type claims have complete actual children or a real failing child. A
 *   reference to an active type proves a cycle, never positive sendability.
 */
enum { ERROR_MARKER_ERROR=1,ERROR_MARKER_SENDABLE=2,ERROR_MARKER_PAIR=3 };
typedef struct ErrorMarkerGoal { uint32_t scope,kind,left,right,binder,bound; } ErrorMarkerGoal;
typedef struct ErrorMarkerNode {
    ErrorMarkerGoal goal;
    uint32_t parent,ordinal,end,reference,children,value;
} ErrorMarkerNode;
typedef struct ErrorMarkerRoot { uint32_t scope,atom,first; } ErrorMarkerRoot;
struct EffectErrorMarkers {
    XrCompileResources *resources;
    ErrorMarkerNode *nodes;
    ErrorMarkerRoot *roots;
    uint32_t count,capacity,root_count,root_capacity;
};
typedef struct ErrorMarkerRule { uint32_t children,value;bool terminal; } ErrorMarkerRule;
typedef struct ErrorMarkerEnum {
    const XrXirNominalDeclaration *declaration;
    const XrXirNominalIdentity *identity;
    uint32_t fields,arity,bounds;
    bool projected;
} ErrorMarkerEnum;
static bool error_marker_goal_equal(const ErrorMarkerGoal *a,const ErrorMarkerGoal *b) {
    return a->scope==b->scope && a->kind==b->kind && a->left==b->left && a->right==b->right &&
        a->binder==b->binder && a->bound==b->bound;
}
static ErrorMarkerGoal error_marker_type_goal(uint32_t scope,uint32_t kind,XrXirType type) {
    return (ErrorMarkerGoal){scope,kind,(uint32_t)type,0,UINT32_MAX,0};
}
static ErrorMarkerGoal error_marker_pair_goal(uint32_t scope,XrXirType source,XrXirType target,uint32_t binder,bool bound) {
    return (ErrorMarkerGoal){scope,ERROR_MARKER_PAIR,(uint32_t)source,(uint32_t)target,bound?binder:UINT32_MAX,bound?1u:0u};
}
static XrXirStatus error_marker_scope(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    uint32_t scope,const XrXirGeneric **output) {
    if(scope>=proof->contexts) return XR_XIR_BAD_STRUCTURE;
    uint32_t function=UINT32_MAX;
    XrXirStatus status=error_finite_context_body(work,proof,scope,&function);
    if(status!=XR_XIR_OK) return status;
    const XrXirGeneric *generics=proof->material->generics;
    if(!generics) { *output=NULL;return XR_XIR_OK; }
    status=effect_role_range(work,generics,proof->functions,sizeof(*generics));if(status!=XR_XIR_OK) return status;
    const XrXirGeneric *generic=&generics[function];
    status=effect_role_range(work,generic->constraints,generic->parameter_count,sizeof(*generic->constraints));
    if(status==XR_XIR_OK) *output=generic;
    return status;
}
static XrXirStatus error_marker_parameter(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    uint32_t scope,XrXirType type,uint32_t *markers) {
    const XrXirGeneric *generic=NULL;XrXirStatus status=error_marker_scope(work,proof,scope,&generic);
    if(status!=XR_XIR_OK) return status;
    uint32_t parameter=(uint32_t)type-XR_XIR_TYPE_PARAMETER_BASE;
    if(!generic || parameter>=generic->parameter_count) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    uint32_t value=generic->constraints[parameter].markers;
    if(value&(XR_XIR_CONSTRAINT_ATOMIC_VALUE|XR_XIR_CONSTRAINT_ATOMIC_NUMBER|XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN))
        value|=XR_XIR_CONSTRAINT_SENDABLE;
    *markers=value;return XR_XIR_OK;
}
static XrXirStatus error_marker_type_node(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    XrXirType type,const XrXirTypeNode **output) {
    const XrXirTypes *types=proof->material->domain.types;
    XrXirStatus status=effect_role_range(work,types->nodes,types->count,sizeof(*types->nodes));if(status!=XR_XIR_OK) return status;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    uint32_t id=(uint32_t)type;
    if(!node && id>=XR_XIR_CONSTRUCTED_TYPE_BASE && id<XR_XIR_CONSTRUCTED_TYPE_LIMIT) return XR_XIR_BAD_STRUCTURE;
    if(node && (node->kind<XR_XIR_TYPE_CALLABLE || node->kind>XR_XIR_TYPE_TASK)) return XR_XIR_BAD_STRUCTURE;
    if(node) {
        status=effect_role_range(work,node->parameters,node->parameter_count,sizeof(*node->parameters));
        if(status==XR_XIR_OK) status=effect_role_range(work,node->nominal.arguments,node->nominal.argument_count,sizeof(*node->nominal.arguments));
        if(status==XR_XIR_OK) status=effect_role_range(work,node->nominal.fields,node->nominal.field_count,sizeof(*node->nominal.fields));
        if(status!=XR_XIR_OK) return status;
    }
    *output=node;return XR_XIR_OK;
}
static XrXirStatus error_marker_enum(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    const XrXirTypeNode *node,ErrorMarkerEnum *output,bool *is_enum) {
    const XrXirNominalTable *table=proof->material->domain.types->nominals;
    XrXirStatus status=effect_role_range(work,table,1,sizeof(*table));if(status!=XR_XIR_OK) return status;
    if(!!table->declarations==!!table->identities || node->nominal.declaration>=table->count) return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalDeclaration *declaration=NULL;const XrXirNominalIdentity *identity=NULL;
    if(table->declarations) {
        status=effect_role_range(work,table->declarations,table->count,sizeof(*table->declarations));
        if(status==XR_XIR_OK) declaration=&table->declarations[node->nominal.declaration];
    } else {
        status=effect_role_range(work,table->identities,table->count,sizeof(*table->identities));
        if(status==XR_XIR_OK) identity=&table->identities[node->nominal.declaration];
    }
    if(status!=XR_XIR_OK) return status;
    uint32_t kind=declaration?declaration->kind:identity->kind;*is_enum=kind==XR_XIR_NOMINAL_ENUM;
    if(!*is_enum) return XR_XIR_OK;
    uint32_t fields=declaration?declaration->field_count:identity->field_count;
    uint32_t arity=declaration?declaration->parameter_count:identity->arity;
    uint32_t variants=declaration?declaration->variant_count:identity->variant_count;
    const XrXirNominalVariant *items=declaration?declaration->variants:identity->variants;
    if(!variants || !items || node->nominal.argument_count!=arity || !!node->nominal.arguments!=!!arity ||
        !!node->nominal.fields!=!!node->nominal.field_count) return XR_XIR_BAD_STRUCTURE;
    if(node->nominal.field_count!=fields && (!declaration || arity || node->nominal.field_count || (fields && !declaration->fields)))
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,items,variants,sizeof(*items));
    if(status==XR_XIR_OK && declaration) status=effect_role_range(work,declaration->fields,fields,sizeof(*declaration->fields));
    if(status!=XR_XIR_OK) return status;
    uint32_t end=0;
    for(uint32_t i=0;i<variants;++i) {
        if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if(items[i].field_begin!=end || items[i].field_count>fields-end) return XR_XIR_BAD_STRUCTURE;
        end+=items[i].field_count;
    }
    if(end!=fields || fields>UINT32_MAX/2) return XR_XIR_BAD_STRUCTURE;
    uint32_t bounds=0;
    if(declaration && arity) {
        status=effect_role_range(work,declaration->constraints,arity,sizeof(*declaration->constraints));
        if(status!=XR_XIR_OK) return status;
        for(uint32_t a=0;a<arity;++a) {
            if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
            const XrXirConstraint *constraint=&declaration->constraints[a];
            if(constraint->interfaces || constraint->interface_count || (constraint->markers&~XR_XIR_CONSTRAINT_SENDABLE))
                return XR_XIR_BAD_STRUCTURE;
            if(constraint->markers&XR_XIR_CONSTRAINT_SENDABLE) ++bounds;
        }
    }
    if(bounds>UINT32_MAX-fields*2) return XR_XIR_BAD_STRUCTURE;
    *output=(ErrorMarkerEnum){declaration,identity,fields,arity,bounds,declaration && node->nominal.field_count};return XR_XIR_OK;
}
static XrXirStatus error_marker_rule(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    const ErrorMarkerGoal *goal,ErrorMarkerRule *output) {
    if(goal->kind<ERROR_MARKER_ERROR || goal->kind>ERROR_MARKER_PAIR || goal->bound>1 ||
        (goal->kind!=ERROR_MARKER_PAIR && (goal->right || goal->bound || goal->binder!=UINT32_MAX))) return XR_XIR_BAD_STRUCTURE;
    const XrXirGeneric *generic=NULL;XrXirStatus status=error_marker_scope(work,proof,goal->scope,&generic);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(work,sizeof(*output)+sizeof(*goal))) return XR_XIR_BUDGET;
    *output=(ErrorMarkerRule){.terminal=true};
    XrXirType type=(XrXirType)goal->left;uint32_t id=goal->left;
    bool parameter=id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT;
    const XrXirTypeNode *node=NULL;status=error_marker_type_node(work,proof,type,&node);if(status!=XR_XIR_OK) return status;
    if(goal->kind==ERROR_MARKER_PAIR) {
        if(!goal->bound && goal->binder!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        if(parameter && goal->bound) {
            const XrXirTypeNode *binder=NULL;status=error_marker_type_node(work,proof,(XrXirType)goal->binder,&binder);
            if(status!=XR_XIR_OK) return status;
            if(!binder || binder->kind!=XR_XIR_TYPE_NOMINAL || id-XR_XIR_TYPE_PARAMETER_BASE>=binder->nominal.argument_count)
                return XR_XIR_BAD_STRUCTURE;
            *output=(ErrorMarkerRule){.children=1};return XR_XIR_OK;
        }
        const XrXirTypeNode *target=NULL;status=error_marker_type_node(work,proof,(XrXirType)goal->right,&target);
        if(status!=XR_XIR_OK) return status;
        if(!node || !target) {
            if(goal->left!=goal->right) return XR_XIR_BAD_STRUCTURE;
            if(parameter && (!generic || id-XR_XIR_TYPE_PARAMETER_BASE>=generic->parameter_count)) return XR_XIR_BAD_STRUCTURE;
            output->value=1;return XR_XIR_OK;
        }
        if(node->kind!=target->kind || node->flags!=target->flags) return XR_XIR_BAD_STRUCTURE;
        if(node->kind==XR_XIR_TYPE_NOMINAL) {
            if(node->nominal.declaration!=target->nominal.declaration || node->nominal.argument_count!=target->nominal.argument_count)
                return XR_XIR_BAD_STRUCTURE;
            output->children=node->nominal.argument_count;
        } else if(node->kind==XR_XIR_TYPE_TUPLE || node->kind==XR_XIR_TYPE_CALLABLE) {
            if(node->parameter_count!=target->parameter_count || node->parameter_count==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            for(uint32_t p=0;p<node->parameter_count;++p) {
                if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                if(node->parameters[p].mode!=target->parameters[p].mode) return XR_XIR_BAD_STRUCTURE;
            }
            output->children=node->parameter_count+(node->kind==XR_XIR_TYPE_CALLABLE?1u:0u);
        } else output->children=1;
        output->terminal=output->children==0;output->value=output->terminal?1u:0u;return XR_XIR_OK;
    }
    if(node && node->parameter_span>(generic?generic->parameter_count:0)) return XR_XIR_BAD_STRUCTURE;
    if(goal->kind==ERROR_MARKER_ERROR) {
        bool error=type==XR_XIR_ERROR;
        if(parameter) { uint32_t markers=0;status=error_marker_parameter(work,proof,goal->scope,type,&markers);error=(markers&XR_XIR_CONSTRAINT_ERROR)!=0; }
        else if(node && node->kind==XR_XIR_TYPE_NOMINAL) {
            ErrorMarkerEnum view={0};status=error_marker_enum(work,proof,node,&view,&error);
        }
        if(status!=XR_XIR_OK) return status;
        if(error) *output=(ErrorMarkerRule){.children=1};return XR_XIR_OK;
    }
    if(type==XR_XIR_UNIT || type==XR_XIR_BOOL || type==XR_XIR_RUNE || xr_xir_type_is_number(type) || type==XR_XIR_STRING) {
        output->value=1;return XR_XIR_OK;
    }
    if(parameter) {
        uint32_t markers=0;status=error_marker_parameter(work,proof,goal->scope,type,&markers);
        if(status==XR_XIR_OK) output->value=(markers&XR_XIR_CONSTRAINT_SENDABLE)!=0;
        return status;
    }
    if(!node) return type==XR_XIR_ERROR || type==XR_XIR_PANIC_INFO?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(node->kind==XR_XIR_TYPE_ATOMIC) {
        output->value=node->element==XR_XIR_I64 || node->element==XR_XIR_F64 || node->element==XR_XIR_BOOL;return XR_XIR_OK;
    }
    if(node->kind==XR_XIR_TYPE_CELL || node->kind==XR_XIR_TYPE_CALLABLE) return XR_XIR_OK;
    if(node->kind==XR_XIR_TYPE_NOMINAL) {
        ErrorMarkerEnum view={0};bool is_enum=false;status=error_marker_enum(work,proof,node,&view,&is_enum);
        if(status!=XR_XIR_OK || !is_enum) return status;
        output->children=view.bounds+view.fields*(view.projected?2u:1u);
    } else if(node->kind==XR_XIR_TYPE_TUPLE) {
        if(!node->parameter_count || !node->parameters) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<node->parameter_count;++p) {
            if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            if(node->parameters[p].mode) return XR_XIR_OK;
        }
        output->children=node->parameter_count;
    } else output->children=1;
    output->terminal=output->children==0;output->value=output->terminal?1u:0u;return XR_XIR_OK;
}
static XrXirStatus error_marker_child(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    const ErrorMarkerGoal *goal,uint32_t ordinal,ErrorMarkerGoal *output) {
    ErrorMarkerRule rule;XrXirStatus status=error_marker_rule(work,proof,goal,&rule);if(status!=XR_XIR_OK) return status;
    if(rule.terminal || ordinal>=rule.children) return XR_XIR_BAD_STRUCTURE;
    if(goal->kind==ERROR_MARKER_ERROR) { *output=error_marker_type_goal(goal->scope,ERROR_MARKER_SENDABLE,(XrXirType)goal->left);return XR_XIR_OK; }
    const XrXirTypeNode *node=NULL;status=error_marker_type_node(work,proof,(XrXirType)goal->left,&node);if(status!=XR_XIR_OK) return status;
    if(goal->kind==ERROR_MARKER_PAIR) {
        if(goal->bound && goal->left>=XR_XIR_TYPE_PARAMETER_BASE && goal->left<XR_XIR_TYPE_PARAMETER_LIMIT) {
            const XrXirTypeNode *binder=NULL;status=error_marker_type_node(work,proof,(XrXirType)goal->binder,&binder);
            if(status!=XR_XIR_OK || !binder) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
            *output=error_marker_pair_goal(goal->scope,binder->nominal.arguments[goal->left-XR_XIR_TYPE_PARAMETER_BASE],(XrXirType)goal->right,UINT32_MAX,false);return XR_XIR_OK;
        }
        const XrXirTypeNode *target=NULL;status=error_marker_type_node(work,proof,(XrXirType)goal->right,&target);
        if(status!=XR_XIR_OK || !node || !target) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
        XrXirType left,right;
        if(node->kind==XR_XIR_TYPE_NOMINAL) { left=node->nominal.arguments[ordinal];right=target->nominal.arguments[ordinal]; }
        else if(node->kind==XR_XIR_TYPE_CALLABLE && !ordinal) { left=node->result;right=target->result; }
        else if(node->kind==XR_XIR_TYPE_CALLABLE || node->kind==XR_XIR_TYPE_TUPLE) {
            uint32_t at=ordinal-(node->kind==XR_XIR_TYPE_CALLABLE?1u:0u);left=node->parameters[at].type;right=target->parameters[at].type;
        } else { left=node->element;right=target->element; }
        *output=error_marker_pair_goal(goal->scope,left,right,goal->binder,goal->bound!=0);return XR_XIR_OK;
    }
    if(!node) return XR_XIR_BAD_STRUCTURE;
    XrXirType child;
    if(node->kind==XR_XIR_TYPE_NOMINAL) {
        ErrorMarkerEnum view={0};bool is_enum=false;status=error_marker_enum(work,proof,node,&view,&is_enum);
        if(status!=XR_XIR_OK || !is_enum) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
        if(ordinal<view.bounds) {
            uint32_t bound=0;
            for(uint32_t a=0;a<view.arity;++a) {
                if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                if(view.declaration->constraints[a].markers&XR_XIR_CONSTRAINT_SENDABLE) {
                    if(bound++==ordinal) {
                        *output=error_marker_type_goal(goal->scope,ERROR_MARKER_SENDABLE,node->nominal.arguments[a]);
                        return XR_XIR_OK;
                    }
                }
            }
            return XR_XIR_BAD_STRUCTURE;
        }
        ordinal-=view.bounds;
        uint32_t at=view.projected?ordinal/2:ordinal;
        child=node->nominal.field_count?node->nominal.fields[at]:view.declaration->fields[at].type;
        if(view.projected && !(ordinal&1)) {
            *output=error_marker_pair_goal(goal->scope,view.declaration->fields[at].type,child,goal->left,true);return XR_XIR_OK;
        }
    } else if(node->kind==XR_XIR_TYPE_TUPLE) child=node->parameters[ordinal].type;
    else child=node->element;
    *output=error_marker_type_goal(goal->scope,ERROR_MARKER_SENDABLE,child);return XR_XIR_OK;
}
static XrXirStatus error_marker_nodes_match(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    const EffectErrorMarkers *markers=proof->markers;
    XrXirStatus status=effect_role_range(work,markers,1,sizeof(*markers));if(status!=XR_XIR_OK) return status;
    if(markers->resources!=work->resources || markers->count>markers->capacity || markers->root_count>markers->root_capacity ||
        !!markers->nodes!=!!markers->capacity || !!markers->roots!=!!markers->root_capacity) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,markers->nodes,markers->capacity,sizeof(*markers->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(work,markers->roots,markers->root_capacity,sizeof(*markers->roots));
    if(status!=XR_XIR_OK) return status;
    for(uint32_t i=0;i<markers->count;++i) {
        const ErrorMarkerNode *node=&markers->nodes[i];
        if(!xir_compile_work(work,sizeof(*node))) return XR_XIR_BUDGET;
        if(node->value>1 || node->end<=i || node->end>markers->count) return XR_XIR_BAD_STRUCTURE;
        if(node->reference!=UINT32_MAX) {
            if(node->reference>=i || node->children || node->end!=i+1) return XR_XIR_BAD_STRUCTURE;
            const ErrorMarkerNode *other=&markers->nodes[node->reference];
            if(!error_marker_goal_equal(&node->goal,&other->goal)) return XR_XIR_BAD_STRUCTURE;
            if(other->end>i) {
                if(node->goal.kind==ERROR_MARKER_PAIR) { if(!node->value) return XR_XIR_BAD_STRUCTURE; }
                else if(node->value) return XR_XIR_BAD_STRUCTURE;
                uint32_t parent=node->parent;bool ancestor=false;
                for(uint32_t walked=0;parent!=UINT32_MAX && walked<=i;++walked) {
                    if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                    if(parent>=i) return XR_XIR_BAD_STRUCTURE;
                    if(parent==node->reference) { ancestor=true;break; }
                    parent=markers->nodes[parent].parent;
                }
                if(!ancestor) return XR_XIR_BAD_STRUCTURE;
            } else if(node->value!=other->value) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        ErrorMarkerRule rule;status=error_marker_rule(work,proof,&node->goal,&rule);if(status!=XR_XIR_OK) return status;
        if(rule.terminal) {
            if(node->children || node->end!=i+1 || node->value!=rule.value) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        if(!node->children || node->children>rule.children || (node->value && node->children!=rule.children)) return XR_XIR_BAD_STRUCTURE;
        uint32_t at=i+1,last=1;
        for(uint32_t c=0;c<node->children;++c) {
            if(at>=node->end) return XR_XIR_BAD_STRUCTURE;
            const ErrorMarkerNode *child=&markers->nodes[at];ErrorMarkerGoal expected;
            status=error_marker_child(work,proof,&node->goal,c,&expected);if(status!=XR_XIR_OK) return status;
            if(child->parent!=i || child->ordinal!=c || child->end<=at || child->end>node->end ||
                !error_marker_goal_equal(&expected,&child->goal)) return XR_XIR_BAD_STRUCTURE;
            last=child->value;
            if(!last && (c+1!=node->children || node->value || node->goal.kind==ERROR_MARKER_PAIR)) return XR_XIR_BAD_STRUCTURE;
            at=child->end;
        }
        if(at!=node->end || (!node->value && last)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_marker_permissions_fold(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    const XrXirStatus *permissions,XrXirStatus *projection) {
    if(!!permissions==!!projection) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_marker_nodes_match(work,proof);if(status!=XR_XIR_OK) return status;
    const EffectErrorMarkers *markers=proof->markers;EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};
    uint32_t root=0,next=0;
    for(uint32_t f=0;f<proof->contexts;++f) {
        uint64_t word=0;status=error_finite_word(&read,proof->rows[f].end-(uint64_t)proof->words*64,64,&word);
        if(status!=XR_XIR_OK) return status;
        XrXirStatus permission=(word&3)?XR_XIR_BAD_TYPE:XR_XIR_OK;
        for(uint32_t a=0;permission==XR_XIR_OK && a<proof->atom_count;++a) {
            if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
            status=error_finite_word(&read,proof->rows[f].end-(uint64_t)proof->words*64+(uint64_t)((a+2)/64)*64,64,&word);
            if(status!=XR_XIR_OK) return status;
            if(!(word&(UINT64_C(1)<<((a+2)%64)))) continue;
            if(root>=markers->root_count || next>=markers->count) return XR_XIR_BAD_STRUCTURE;
            const ErrorMarkerRoot *entry=&markers->roots[root];const ErrorMarkerNode *node=&markers->nodes[next];
            ErrorMarkerGoal expected=error_marker_type_goal(f,ERROR_MARKER_ERROR,proof->atoms[a].type);
            if(entry->scope!=f || entry->atom!=a || entry->first!=next || node->parent!=UINT32_MAX || node->ordinal!=root ||
                !error_marker_goal_equal(&expected,&node->goal)) return XR_XIR_BAD_STRUCTURE;
            if(!node->value) permission=XR_XIR_BAD_TYPE;
            next=node->end;++root;
        }
        if(f<proof->functions) {
            if(projection) {
                if(!xir_compile_work(work,sizeof(*projection))) return XR_XIR_BUDGET;
                projection[f]=permission;
            } else if(permissions[f]!=permission) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return root==markers->root_count && next==markers->count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus error_marker_permissions_match(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    const XrXirStatus *permissions) {
    return error_marker_permissions_fold(work,proof,permissions,NULL);
}
