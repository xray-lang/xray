/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_storage.inc.c - One final same-ledger body snapshot
 */
#include "xxir_effect_lowered_snapshot.inc.c"
#include "xxir_effect_invocation_declarations.inc.c"

#include "xxir_effect_snapshot_metadata_match.inc.c"

static void effect_body_material_free(EffectBodyMaterial *body) {
    if (!body) return;
    if (body->functions) for (uint32_t f=0;f<effect_body_count(body);++f)
        xr_compile_resources_free(body->functions[f].stream);
    effect_physical_free(body->physical);
    xr_xir_compile_cell_provenance_free(body->cells);
    effect_invocation_bounds_free(body->declared);
    effect_invocation_declarations_free(body->declarations);
    xr_xir_compile_generics_free(body->generics,effect_body_count(body));
    effect_terms_free(&body->terms);
    xr_xir_compile_types_free(body->owned_types);
    xr_compile_resources_free(body);
}

/* All admitted slices share the material lifetime. No destructor walks a
 * partially initialized child array; failures release complete owners only. */
static void *effect_body_copy(EffectTerms *terms,const void *source,uint64_t count,size_t size) {
    if (!count || terms->status!=XR_XIR_OK) return NULL;
    if (!source || !size) { terms->status=XR_XIR_BAD_STRUCTURE;return NULL; }
    void *copy=effect_terms_reserve(terms,count,size);
    if (!copy) return NULL;
    if (!xir_compile_work(terms->remaining,count*size)) { terms->status=XR_XIR_BUDGET;return NULL; }
    memcpy(copy,source,(size_t)count*size);return copy;
}

/* One complete physical type owner preserves every descriptor child in
 * every stage. Semantic domains are still proved by the common matcher;
 * the representation does not erase executable fields from a snapshot. */
static XrXirStatus effect_body_types_copy(EffectBodyMaterial *body,const XrXirModule *source) {
    EffectTerms *terms=&body->terms;
    const XrXirTypes *types=source->types?source->types:&effect_empty_types;
    if (!xir_compile_work(terms->remaining,sizeof(*types))) return XR_XIR_BUDGET;
    if ((types->count!=0)!=(types->nodes!=NULL)) return XR_XIR_BAD_STRUCTURE;
    if (source->stage==XR_XIR_LOWERED && (types->interfaces ||
        (types->nominals && (!types->nominals->count || types->nominals->declarations ||
         !types->nominals->identities)))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    if (!types->count && !types->nominals && !types->interfaces) {
        terms->types=(XrXirTypes){0};terms->capacity=0;
    } else {
        status=xr_xir_compile_types_clone(terms->remaining,types,&body->owned_types);
        if (status==XR_XIR_OK) { terms->types=*body->owned_types;terms->capacity=types->count; }
    }
    if (status==XR_XIR_OK) body->domain.types=&terms->types;
    return status;
}

/* Semantic shape comparison and full physical node correspondence have
 * different responsibilities. This exact comparison retains open parameter
 * spans as well as executable fields and all physical child identities. */
static XrXirStatus effect_body_type_nodes_match(const XrXirCompileContext *work,
    const XrXirTypes *owned,const XrXirTypes *current) {
    if (!owned || !current || owned->count!=current->count ||
        (owned->count && (!owned->nodes || !current->nodes))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t t=0;t<owned->count;++t) {
        const XrXirTypeNode *a=&owned->nodes[t],*b=&current->nodes[t];
        if (!xir_compile_work(work,2*sizeof(*a))) return XR_XIR_BUDGET;
        if (a->kind!=b->kind || a->element!=b->element || a->result!=b->result ||
            a->flags!=b->flags || a->parameter_span!=b->parameter_span ||
            a->parameter_count!=b->parameter_count || a->nominal.declaration!=b->nominal.declaration ||
            a->nominal.argument_count!=b->nominal.argument_count ||
            a->nominal.field_count!=b->nominal.field_count ||
            (a->parameter_count && (!a->parameters || !b->parameters))) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;p<a->parameter_count;++p) {
            if (!xir_compile_work(work,2*sizeof(*a->parameters))) return XR_XIR_BUDGET;
            if (a->parameters[p].mode!=b->parameters[p].mode || a->parameters[p].type!=b->parameters[p].type)
                return XR_XIR_BAD_STRUCTURE;
        }
        XrXirStatus status=effect_lowered_snapshot_vector(work,a->nominal.arguments,
            b->nominal.arguments,a->nominal.argument_count);
        if (status==XR_XIR_OK) status=effect_lowered_snapshot_vector(work,a->nominal.fields,
            b->nominal.fields,a->nominal.field_count);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_body_function_copy(EffectTerms *terms,
    const XrXirFunction *source,EffectBodyFunction *target) {
    if (!xir_compile_work(terms->remaining,11+sizeof(*target))) return XR_XIR_BUDGET;
    *target=(EffectBodyFunction){.name_length=source->name_length,
        .parameter_count=source->parameter_count,.result=source->result,
        .block_count=source->block_count,.instruction_count=source->instruction_count,
        .operand_count=source->operand_count};
    target->parameters=effect_body_copy(terms,source->parameters,source->parameter_count,sizeof(*source->parameters));
    target->blocks=effect_body_copy(terms,source->blocks,source->block_count,sizeof(*source->blocks));
    target->operands=effect_body_copy(terms,source->operands,source->operand_count,sizeof(*source->operands));
    target->name=effect_body_copy(terms,source->name,source->name_length,1);
    if (terms->status!=XR_XIR_OK) return terms->status;
    return XR_XIR_OK;
}

static XrXirStatus effect_body_metadata_copy(EffectBodyMaterial *body,const XrXirModule *source) {
    EffectTerms *terms=&body->terms;uint32_t count=source->function_count;
    XrXirStatus status=effect_invocation_declarations_clone(terms->remaining,source->declarations,
        count,&body->declarations);
    if (status==XR_XIR_OK && source->generics)
        status=xr_xir_compile_generics_clone(terms->remaining,source,&body->generics);
    if (status==XR_XIR_OK && source->defaults) {
        body->defaults.records=effect_body_copy(terms,source->defaults->records,
            source->defaults->count,sizeof(*source->defaults->records));
        body->defaults.count=source->defaults->count;status=terms->status;
        if (status==XR_XIR_OK) body->domain.defaults=&body->defaults;
    }
    if (status==XR_XIR_OK) {
        body->domain.declarations=body->declarations;body->domain.generics=body->generics;
    }
    return status;
}

static bool effect_body_advertisements_span_ready(const EffectBodyAdvertisementBuilder *builder,uint32_t f);
static XrXirStatus effect_body_advertisements_span(EffectBodyAdvertisementBuilder *builder,
    uint32_t f,uint32_t value,uint32_t count,XrXirType actual);

typedef struct EffectBodyWriter {
    const XrXirCompileContext *work;
    const XrXirModule *source;
    EffectBodyMaterial *material;
    XrXirCellProofBuilder *cells;
    EffectBodyAdvertisementBuilder *advertisements;
    uint32_t function;
} EffectBodyWriter;

static XrXirStatus effect_body_writer_row(void *owner,uint32_t instruction,
    const XrXirInstruction *operation) {
    EffectBodyWriter *writer=owner;
    if (!writer || !operation || writer->function>=effect_body_count(writer->material))
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *function=&writer->material->functions[writer->function];
    if (instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(writer->work,1)) return XR_XIR_BUDGET;
    if (!effect_seed(writer->source,&writer->source->functions[writer->function],operation,
        &writer->material->control[writer->function])) return XR_XIR_BAD_STRUCTURE;
    XrXirEffects local={.count=effect_body_count(writer->material),.root=writer->material->local_roots,
        .root_witnesses=writer->material->local_witnesses,
        .unresolved_witnesses=writer->material->local_witnesses+effect_body_count(writer->material)};
    if (operation->op!=XR_XIR_CALL_INDIRECT && operation->op!=XR_XIR_INVOKE_INDIRECT) {
        EffectRootSeedRecord seed={&writer->material->domain,operation,writer->function,instruction,XR_XIR_UNIT};
        XrXirStatus seeded=effect_root_seed_record(&seed,&local,writer->work);
        if (seeded!=XR_XIR_OK) return seeded;
        if (operation->op==XR_XIR_CALL_REQUIREMENT) {
            if (!xir_compile_work(writer->work,1+sizeof(XrXirRootEffectWitness))) return XR_XIR_BUDGET;
            local.root[writer->function].unresolved=false;
            local.unresolved_witnesses[writer->function]=(XrXirRootEffectWitness){0};
        }
    }
    uint32_t value=function->parameter_count+instruction;
    XrXirStatus status=effect_body_advertisements_value(writer->advertisements,writer->function,value,operation->type);
    if (status!=XR_XIR_OK) return status;
    status=effect_physical_value(writer->work,writer->material->domain.types,
        &writer->material->physical->functions[writer->function],value,operation->type,operation->op);
    if (status==XR_XIR_OK)
        status=xir_cell_proof_builder_instruction(writer->cells,writer->function,value,operation->type,
            operation->op,operation->type);
    return status;
}

/* The codec has compared every physical word of every row before this
 * synchronous intake. Its mask only selects a layout opportunity: the actual
 * Source row, independent owned span and genuine semantic kernels still
 * establish inventory. No range publishes a completed provider or NONE fact. */
static XrXirStatus effect_body_writer_span(void *owner,uint32_t first,uint32_t count) {
    EffectBodyWriter *writer=owner;
    if (!writer || writer->function>=effect_body_count(writer->material) || !count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *source=&writer->source->functions[writer->function];
    const EffectBodyFunction *function=&writer->material->functions[writer->function];
    if (first>=source->instruction_count || count>source->instruction_count-first ||
        source->instruction_count!=function->instruction_count || !source->instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *operation=&source->instructions[first];
    if (!xir_compile_work(writer->work,8)) return XR_XIR_BUDGET;
    if (!effect_body_advertisements_span_ready(writer->advertisements,writer->function) ||
        !xir_cell_proof_scalar_shape(operation->op,operation->type)) {
        for (uint32_t i=first;i<first+count;++i) {
            if (!xir_compile_work(writer->work,1)) return XR_XIR_BUDGET;
            XrXirStatus status=effect_body_writer_row(owner,i,&source->instructions[i]);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    if (!xir_compile_work(writer->work,1+sizeof(XrXirCellProofInstructionShape))) return XR_XIR_BUDGET;
    XrXirCellProofInstructionShape shape={operation->op,operation->type};
    XrXirStatus status=xir_cell_proof_builder_span(writer->cells,writer->function,first,count,&shape);
    if (status!=XR_XIR_OK) return status;
    if (!effect_seed(writer->source,source,operation,&writer->material->control[writer->function]))
        return XR_XIR_BAD_STRUCTURE;
    XrXirEffects local={.count=effect_body_count(writer->material),.root=writer->material->local_roots,
        .root_witnesses=writer->material->local_witnesses,
        .unresolved_witnesses=writer->material->local_witnesses+effect_body_count(writer->material)};
    EffectRootSeedRecord seed={&writer->material->domain,operation,writer->function,first,XR_XIR_UNIT};
    status=effect_root_seed_record(&seed,&local,writer->work);
    if (status!=XR_XIR_OK) return status;
    uint32_t value=function->parameter_count+first;
    status=effect_body_advertisements_span(writer->advertisements,writer->function,value,count,operation->type);
    if (status==XR_XIR_OK) status=effect_physical_value(writer->work,writer->material->domain.types,
        &writer->material->physical->functions[writer->function],value,operation->type,operation->op);
    return status;
}

/* Constructor verifies actual TEMPLATE/INSTANCE evidence before any concrete
 * role product. No provenance alias or verified flag survives as admission. */
static XrXirStatus effect_body_material_generation_input(const XrXirCompileContext *work,
    const XrXirModule *source,EffectRefinementBounds *refinement,
    const EffectBodyOrdinaryInput *ordinary,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !source || !output || *output ||
        (!!source->function_count!=!!source->functions)) return XR_XIR_BAD_STRUCTURE;
    if (source->function_count>work->limits.functions) return XR_XIR_BUDGET;
    XrXirStatus status=ordinary ? effect_body_ordinary_input_verify(work,source,ordinary) :
        xir_effect_contract_shape_verify(work,source);
    if (status!=XR_XIR_OK) return status;
    EffectBodyMaterial *body=xir_compile_calloc(work,1,sizeof(*body),&status);
    if (!body) return status;
    body->resources=work->resources;body->terms.remaining=work;
    body->domain.function_count=source->function_count;
    body->stage=source->stage;body->linkage_kind=source->linkage_kind;
    if (!xir_compile_work(work,sizeof(body->evidence_kind))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) body->evidence_kind=source->provenance?source->provenance->kind:0;
    if (status==XR_XIR_OK) status=effect_body_types_copy(body,source);
    if (status==XR_XIR_OK) status=effect_body_metadata_copy(body,source);
    if (status==XR_XIR_OK && ordinary) {
        EffectBodyOrdinaryIdentity *ids=effect_terms_reserve(&body->terms,source->function_count,sizeof(*ids));
        if (!ids) status=body->terms.status;
        for (uint32_t f=0;status==XR_XIR_OK && f<source->function_count;++f)
            status=effect_body_ordinary_identity(work,ordinary,f,&ids[f]);
        if (status==XR_XIR_OK) {
            if (!xir_compile_work(work,2)) status=XR_XIR_BUDGET;
            else { body->ordinary_identities=ids;body->ordinary_source_count=ordinary->source->function_count; }
        }
    }
    if (status==XR_XIR_OK) {
        body->functions=effect_terms_alloc(&body->terms,source->function_count,sizeof(*body->functions));
        if (source->function_count && !body->functions) status=body->terms.status;
    }
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        status=effect_body_function_copy(&body->terms,&source->functions[f],&body->functions[f]);
        if (status!=XR_XIR_OK) break;
        EffectBodyFunction *function=&body->functions[f];
        uint8_t *roles=effect_terms_reserve(&body->terms,function->parameter_count,sizeof(*roles));
        if (function->parameter_count && !roles) { status=body->terms.status;break; }
        function->initial_roles=roles;
        bool variables=effect_body_variable_roles(body);
        for (uint32_t p=0;p<function->parameter_count && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(work,2+sizeof(*roles))) { status=XR_XIR_BUDGET;break; }
            roles[p]=variables && xr_xir_callable_signature(body->domain.types,function->parameters[p]) ?
                XR_XIR_EFFECT_PARAMETER_VARIABLE:XR_XIR_EFFECT_PARAMETER_FIXED;
        }
    }
    if (status==XR_XIR_OK) {
        body->physical=xir_compile_calloc(work,1,sizeof(*body->physical),&status);
        if (body->physical) {
            body->physical->resources=work->resources;
            body->physical->functions=source->function_count?xir_compile_calloc(work,source->function_count,
                sizeof(*body->physical->functions),&status):NULL;
            if (status==XR_XIR_OK) body->physical->count=source->function_count;
        }
    }
    if (status==XR_XIR_OK) {
        body->control=effect_terms_alloc(&body->terms,source->function_count,sizeof(*body->control));
        body->local_roots=effect_terms_alloc(&body->terms,source->function_count,sizeof(*body->local_roots));
        body->local_witnesses=effect_terms_alloc(&body->terms,(uint64_t)source->function_count*2,
            sizeof(*body->local_witnesses));
        status=body->terms.status;
    }
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};XrXirCellProofBuilder *cells=NULL;
    EffectBodyAdvertisementBuilder *advertisements=NULL;
    if (status==XR_XIR_OK) status=effect_body_advertisements_begin(work,source,refinement,&advertisements);
    if (status==XR_XIR_OK) status=effect_cell_body_owned(work,body,&adapter,&read);
    if (status==XR_XIR_OK) status=xir_cell_proof_builder_begin(work,&read,&cells);
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        EffectBodyFunction *function=&body->functions[f];
        const XrXirDeclarations *declarations=body->domain.declarations;
        if (declarations) {
            if (!declarations->functions || !declarations->modules ||
                declarations->functions[f].module>=declarations->module_count) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            if (declarations->modules[declarations->functions[f].module].initializer==f) {
                if (!xir_compile_work(work,1)) { status=XR_XIR_BUDGET;break; }
                body->local_roots[f].requires_root=true;
                body->local_witnesses[f]=(XrXirRootEffectWitness){.cause=XR_XIR_ROOT_CAUSE_INITIALIZER,
                    .instruction=UINT32_MAX,.callee=UINT32_MAX,.slot=UINT32_MAX};
            }
        }
        uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
        if (values>UINT32_MAX) { status=XR_XIR_BUDGET;break; }
        if (!xir_compile_work(work,3+sizeof(XrXirCellProofBodyHead))) { status=XR_XIR_BUDGET;break; }
        EffectPhysicalFunction *physical=&body->physical->functions[f];
        physical->parameters=function->parameter_count;physical->instructions=function->instruction_count;
        physical->values=(uint32_t)values;
        XrXirCellProofBodyHead head={function->parameters,function->parameter_count,function->result,
            function->instruction_count,function->operands,function->operand_count};
        status=effect_body_advertisements_function(advertisements,f);
        if (status==XR_XIR_OK) status=xir_cell_proof_builder_head(cells,f,&head);
        for (uint32_t p=0;p<function->parameter_count && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
            status=effect_body_advertisements_value(advertisements,f,p,function->parameters[p]);
            if (status==XR_XIR_OK) status=effect_physical_value(work,body->domain.types,physical,p,function->parameters[p],XR_XIR_INVALID);
            if (status==XR_XIR_OK) status=xir_cell_proof_builder_parameter(cells,f,p,function->parameters[p]);
        }
        EffectBodyWriter writer={work,source,body,cells,advertisements,f};EffectBodyWriteSink sink={&writer,effect_body_writer_row,effect_body_writer_span};
        if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(sink.span))) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) status=effect_body_records_write(work,&source->functions[f],function,&sink);
        if (status==XR_XIR_OK) status=xir_cell_proof_builder_function(cells,f);
    }
    if (status==XR_XIR_OK) status=xir_cell_proof_builder_finish(&cells,&body->cells);
    if (status==XR_XIR_OK) status=effect_body_advertisements_finish(&advertisements,&body->declared);
    effect_body_advertisements_free(advertisements);xir_cell_proof_builder_free(cells);
    if (status!=XR_XIR_OK) { effect_body_material_free(body);return status; }
    if (!xir_compile_work(work,1)) { effect_body_material_free(body);return XR_XIR_BUDGET; }
    *output=body;return XR_XIR_OK;
}

/* Public raw construction preserves the complete original evidence admission. */
static XrXirStatus effect_body_material_generation(const XrXirCompileContext *work,
    const XrXirModule *source,EffectRefinementBounds *refinement,EffectBodyMaterial **output) {
    return effect_body_material_generation_input(work,source,refinement,NULL,output);
}

/* Literal construction uses the same writer without a Source refiner. */
static inline XrXirStatus effect_body_material_create(const XrXirCompileContext *work,
    const XrXirModule *source,EffectBodyMaterial **output) {
    return effect_body_material_generation(work,source,NULL,output);
}

/* Original metadata and physical body producers have no lifetime claim on
 * this receiving owner. Fresh external source matches remain full traversals. */
static XrXirStatus effect_body_function_match(const XrXirCompileContext *work,
    const EffectBodyFunction *body,const XrXirFunction *source) {
    EffectBodyRead read=effect_body_compile_read(work);
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (source->name_length!=body->name_length || source->parameter_count!=body->parameter_count ||
        source->result!=body->result || source->block_count!=body->block_count ||
        source->instruction_count!=body->instruction_count || source->operand_count!=body->operand_count)
        return XR_XIR_BAD_STRUCTURE;
    const void *left[4]={source->name,source->parameters,source->blocks,source->operands};
    const void *right[4]={body->name,body->parameters,body->blocks,body->operands};
    uint64_t bytes[4]={source->name_length,(uint64_t)source->parameter_count*sizeof(*source->parameters),
        (uint64_t)source->block_count*sizeof(*source->blocks),(uint64_t)source->operand_count*sizeof(*source->operands)};
    for (uint32_t p=0;p<4;++p) {
        if (bytes[p]>SIZE_MAX) return XR_XIR_BUDGET;
        if (!xir_compile_work(work,bytes[p]+1)) return XR_XIR_BUDGET;
        if (bytes[p] && (!left[p] || !right[p] || memcmp(left[p],right[p],(size_t)bytes[p])))
            return XR_XIR_BAD_STRUCTURE;
    }
    return effect_body_records_match(&read,body,source);
}

/* The original seal boundary proves complete type/domain correspondence.
 * Its single writer already consumed every source body field. It does not
 * introduce a second body traversal after that write. */
static XrXirStatus effect_body_type_domain_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,const XrXirModule *source) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || !source ||
        body->resources!=work->resources || source->stage!=body->stage ||
        source->linkage_kind!=body->linkage_kind ||
        source->function_count!=effect_body_count(body) ||
        (source->function_count && (!source->functions || !body->functions)))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *types=source->types ? source->types : &effect_empty_types;
    EffectTerms terms={.types=*body->domain.types,.remaining=work};
    XrXirStatus status=source->stage==XR_XIR_LOWERED ?
        effect_lowered_snapshot_match(work,&body->terms.types,types) :
        effect_terms_snapshot_match(&terms,types,types->count);
    if (status==XR_XIR_OK && source->stage!=XR_XIR_LOWERED)
        status=effect_body_type_nodes_match(work,&body->terms.types,types);
    effect_terms_free(&terms);
    return status;
}

/* Error/source fresh correspondence retains the complete independent body
 * and metadata traversal. Both lifetime adapters use the same sole reader;
 * no pointer identity substitutes for any source field comparison. */
static XrXirStatus effect_body_source_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,const XrXirModule *source) {
    if (!effect_body_material_shape(body) || body->ordinary_identities) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_type_domain_match(work,body,source);
    if (status==XR_XIR_OK) status=xir_effect_contract_shape_verify(work,source);
    if (status!=XR_XIR_OK) return status;
    uint32_t kind=source->provenance?source->provenance->kind:0;
    if (status==XR_XIR_OK && !xir_compile_work(work,1)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK && kind!=body->evidence_kind) status=XR_XIR_BAD_STRUCTURE;
    bool variables=kind==XR_XIR_EVIDENCE_TEMPLATE || kind==XR_XIR_EVIDENCE_INSTANCE;
    for (uint32_t f=0;status==XR_XIR_OK && f<source->function_count;++f) {
        status=effect_body_function_match(work,&body->functions[f],&source->functions[f]);
        const EffectBodyFunction *function=&body->functions[f];
        if (status==XR_XIR_OK && function->parameter_count && !function->initial_roles)
            status=XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;status==XR_XIR_OK && p<function->parameter_count;++p) {
            if (!xir_compile_work(work,3)) { status=XR_XIR_BUDGET;break; }
            uint32_t role=variables && xr_xir_callable_signature(body->domain.types,function->parameters[p]) ?
                XR_XIR_EFFECT_PARAMETER_VARIABLE:XR_XIR_EFFECT_PARAMETER_FIXED;
            if (function->initial_roles[p]!=role) status=XR_XIR_BAD_STRUCTURE;
        }
    }
    if (status==XR_XIR_OK) status=effect_body_metadata_match(work,&body->domain,source);
    return status;
}

/* This adapter replays the unchanged exhaustive physical opcode classifier.
 * It uses the sole immutable reader, never an executable Module facsimile. */
static XrXirStatus effect_body_physical_match(const XrXirCompileContext *work,
    const EffectPhysicalDomain *domain,const EffectBodyMaterial *material) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(material) || !domain ||
        domain->resources!=work->resources || material->resources!=work->resources ||
        domain->count!=effect_body_count(material) || (domain->count && !domain->functions))
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=effect_body_compile_read(work);
    for (uint32_t f=0;f<domain->count;++f) {
        const EffectPhysicalFunction *body=&domain->functions[f];
        const EffectBodyFunction *function=&material->functions[f];
        if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
        if (body->parameters!=function->parameter_count || body->instructions!=function->instruction_count ||
            body->values!=(uint64_t)function->parameter_count+function->instruction_count ||
            body->rows>body->values || body->classes>body->values || body->count>body->capacity ||
            body->capacity>body->values || !!body->records!=!!body->capacity ||
            (function->parameter_count && !function->parameters)) return XR_XIR_BAD_STRUCTURE;
        uint32_t row=0,classes=0,at=0;
        for (uint32_t v=0;v<body->values;++v) {
            if (!xir_compile_work(work,v<body->parameters?2:3)) return XR_XIR_BUDGET;
            uint32_t mask=0;XrXirType type;
            XrXirStatus status=effect_body_operand_type(&read,function,v,&type);
            if (status!=XR_XIR_OK) return status;
            if (v>=body->parameters) {
                XrXirOp op;
                status=effect_body_instruction_op(&read,function,v-body->parameters,&op);
                if (status!=XR_XIR_OK) return status;
                mask=effect_physical_opcode(op);
            }
            if (xr_xir_callable_signature(material->domain.types,type)) mask|=EFFECT_PHYSICAL_CALLABLE;
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            if (xr_xir_type_is_class(material->domain.types,type)) { mask|=EFFECT_PHYSICAL_CLASS;++classes; }
            if (mask&EFFECT_PHYSICAL_CALLABLE) ++row;
            if (!mask) continue;
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (at>=body->count || body->records[at].value!=v || body->records[at].mask!=mask)
                return XR_XIR_BAD_STRUCTURE;
            ++at;
        }
        if (at!=body->count || row!=body->rows || classes!=body->classes) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
