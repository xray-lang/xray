/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_storage.inc.c - One final same-ledger body snapshot
 */
#include "../base/xchecks.h"
#include "xxir_effect_lowered_snapshot.inc.c"
#include "xxir_effect_invocation_declarations.inc.c"

#include "xxir_effect_snapshot_metadata_match.inc.c"
#include "xxir_effect_snapshot_types_compare.inc.c"
static XrXirStatus effect_body_material_continue(const XrXirCompileContext *work,
    const XrXirModule *source,const EffectBodyOrdinaryInput *ordinary,EffectBodyMaterial **output);

static void effect_body_ordinary_overlay_free(EffectBodyOrdinaryOverlay *overlay);
static XrXirStatus effect_body_material_initial(const XrXirCompileContext *work,
    const XrXirModule *source,const EffectBodyOrdinaryInput *ordinary,EffectBodyMaterial **output);
static void effect_body_material_free(EffectBodyMaterial *body) {
    if (!body) return;
    XR_CHECK(body->references,"effect body reference must be live");
    if (--body->references) return;
    if (body->backbone) {
        XR_CHECK(effect_body_backbone_shape(body),
            "effect body backbone must be a live same-ledger complete owner");
        effect_body_ordinary_overlay_free(body->overlay);
        effect_invocation_bounds_free(body->declared);
        effect_body_material_free(body->backbone);
        xr_compile_resources_free(body);return;
    }
    if (body->functions) for (uint32_t f=0;f<effect_body_count(body);++f)
        effect_body_runs_release(body->functions[f].runs);
    effect_physical_free(body->physical);
    xr_xir_compile_cell_provenance_free(body->cells);
    effect_invocation_bounds_free(body->declared);
    effect_invocation_declarations_free(body->declarations);
    xr_xir_compile_generics_free(body->generics,effect_body_count(body));
    effect_terms_free(&body->terms);
    xr_xir_compile_types_free(body->owned_types);
    effect_input_facts_free(body->input_facts);
    effect_body_material_free(body->canonical_source);
    xr_compile_resources_free(body);
}

/* All admitted slices share the material lifetime. No destructor walks a
 * partially initialized child array; failures release complete owners only. */
static void *effect_body_copy(EffectTerms *terms,const void *source,uint64_t count,size_t size) {
    if (!count || terms->status!=XR_XIR_OK) return NULL;
    if (!source || !size) { terms->status=XR_XIR_BAD_STRUCTURE;return NULL; }
    void *copy=effect_terms_reserve(terms,count,size);
    if (!copy) return NULL;

    memcpy(copy,source,(size_t)count*size);return copy;
}

/* One complete physical type owner preserves every descriptor child in
 * every stage. Semantic domains are still proved by the common matcher;
 * the representation does not erase executable fields from a snapshot. */
static XrXirStatus effect_body_types_copy(EffectBodyMaterial *body,const XrXirModule *source) {
    EffectTerms *terms=&body->terms;
    const XrXirTypes *types=source->types?source->types:&effect_empty_types;

    if ((types->count!=0)!=(types->nodes!=NULL)) return XR_XIR_BAD_STRUCTURE;
    if (source->stage==XR_XIR_LOWERED && (types->interfaces ||
        (types->nominals && (!types->nominals->count || types->nominals->declarations ||
         !types->nominals->identities)))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    if (!types->count && !types->nominals && !types->interfaces) {
        terms->types=(XrXirTypes){0};terms->capacity=0;
    } else {
        status=xr_xir_compile_types_clone(terms->remaining,types, NULL,&body->owned_types);
        if (status==XR_XIR_OK) { terms->types=*body->owned_types;terms->capacity=types->count; }
    }
    if (status==XR_XIR_OK) body->domain.types=&terms->types;
    return status;
}

/* Semantic shape comparison and full physical node correspondence have
 * different responsibilities. This exact comparison retains open parameter
 * spans as well as executable fields and all physical child identities. */
static XrXirStatus effect_body_type_nodes_match(
    const XrXirTypes *owned,const XrXirTypes *current) {
    return effect_body_type_nodes_compare(owned,current,NULL);
}

static XrXirStatus effect_body_function_copy(EffectTerms *terms,
    const XrXirFunction *source,EffectBodyFunction *target) {

    *target=(EffectBodyFunction){.name_length=source->name_length,
        .parameter_count=source->parameter_count,.result=source->result,
        .block_count=source->block_count,.instruction_count=source->instruction_count,
        .operand_count=source->operand_count,.error_type=source->error_type,
        .consume_parameter_count=source->consume_parameter_count};
    target->parameters=effect_body_copy(terms,source->parameters,source->parameter_count,sizeof(*source->parameters));
    target->consume_parameters=effect_body_copy(terms,source->consume_parameters,
        source->consume_parameter_count,sizeof(*source->consume_parameters));
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
        status=xr_xir_compile_generics_clone(terms->remaining,source,&body->generics, NULL);
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

static XrXirStatus effect_root_seed_output(const EffectRootSeedRecord *request,XrXirRootEffects *facts,
    XrXirRootEffectWitness *required,XrXirRootEffectWitness *unresolved,
    const XrXirCompileContext *work,XirReadScratch *memory);

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

    XrXirFunctionEffects local={0};
    if (!effect_seed(writer->source,&writer->source->functions[writer->function],operation,&local))
        return XR_XIR_BAD_STRUCTURE;
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

    if (!xir_cell_proof_scalar_shape(operation->op,operation->type)) {
        for (uint32_t i=first;i<first+count;++i) {

            XrXirStatus status=effect_body_writer_row(owner,i,&source->instructions[i]);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }

    XrXirCellProofInstructionShape shape={operation->op,operation->type};
    XrXirStatus status=xir_cell_proof_builder_span(writer->cells,writer->function,first,count,&shape);
    if (status!=XR_XIR_OK) return status;
    XrXirFunctionEffects local={0};
    if (!effect_seed(writer->source,source,operation,&local))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t value=function->parameter_count+first;
    status=effect_body_advertisements_span(writer->advertisements,writer->function,value,count,operation->type);
    if (status==XR_XIR_OK) status=effect_physical_value(writer->work,writer->material->domain.types,
        &writer->material->physical->functions[writer->function],value,operation->type,operation->op);
    return status;
}

#include "xxir_effect_snapshot_record_sharing.inc.c"

/* Constructor verifies actual TEMPLATE/INSTANCE evidence before any concrete
 * role product. No provenance alias or verified flag survives as admission. */
static XrXirStatus effect_body_material_generation(const XrXirCompileContext *work,
    const XrXirModule *source,EffectRefinementBounds *refinement,
    EffectInputFacts *input_facts,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !source || !output || *output ||
        !input_facts || input_facts->resources!=work->resources ||
        (!!source->function_count!=!!source->functions)) return XR_XIR_BAD_STRUCTURE;
    if (source->function_count>work->limits.functions) return XR_XIR_BUDGET;
    XrXirStatus status=xir_effect_contract_shape_verify(work,source);
    if (status!=XR_XIR_OK) return status;
    /* Prepay the initial reference write and its unavoidable release. */

    EffectBodyMaterial *body=xir_compile_calloc(work,1,sizeof(*body),&status);
    if (!body) return status;
    body->resources=work->resources;body->references=1;body->terms.remaining=work;
    status=effect_input_facts_retain(work,input_facts);
    if (status!=XR_XIR_OK) { effect_body_material_free(body);return status; }
    body->input_facts=input_facts;
    body->domain.function_count=source->function_count;
    body->stage=source->stage;body->linkage_kind=source->linkage_kind;

    if (status==XR_XIR_OK) body->evidence_kind=source->provenance?source->provenance->kind:0;
    if (status==XR_XIR_OK) status=effect_body_types_copy(body,source);
    if (status==XR_XIR_OK) status=effect_body_metadata_copy(body,source);
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
        }
        uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
        if (values>UINT32_MAX) { status=XR_XIR_BUDGET;break; }

        EffectPhysicalFunction *physical=&body->physical->functions[f];
        physical->parameters=function->parameter_count;physical->instructions=function->instruction_count;
        physical->values=(uint32_t)values;
        XrXirCellProofBodyHead head={function->parameters,function->parameter_count,function->result,
            function->instruction_count,function->operands,function->operand_count};
        status=effect_body_advertisements_function(advertisements,f);
        if (status==XR_XIR_OK) status=xir_cell_proof_builder_head(cells,f,&head);
        for (uint32_t p=0;p<function->parameter_count && status==XR_XIR_OK;++p) {

            status=effect_body_advertisements_value(advertisements,f,p,function->parameters[p]);
            if (status==XR_XIR_OK) status=effect_physical_value(work,body->domain.types,physical,p,function->parameters[p],XR_XIR_INVALID);
            if (status==XR_XIR_OK) status=xir_cell_proof_builder_parameter(cells,f,p,function->parameters[p]);
        }
        EffectBodyWriter writer={work,source,body,cells,advertisements,f};EffectBodyWriteSink sink={&writer,effect_body_writer_row,effect_body_writer_span};

        if (status==XR_XIR_OK)
            status=effect_body_records_write(work,&source->functions[f],function,&sink);
        if (status==XR_XIR_OK) status=xir_cell_proof_builder_function(cells,f);
    }
    if (status==XR_XIR_OK) status=xir_cell_proof_builder_finish(&cells,&body->cells);
    if (status==XR_XIR_OK) status=effect_body_advertisements_finish(&advertisements,&body->declared);
    effect_body_advertisements_free(advertisements);xir_cell_proof_builder_free(cells);
    if (status!=XR_XIR_OK) { effect_body_material_free(body);return status; }

    *output=body;return XR_XIR_OK;
}

/* Literal construction uses the same writer without a Source refiner. */

/* Original metadata and physical body producers have no lifetime claim on
 * this receiving owner. Fresh external source matches remain full traversals. */
static XrXirStatus effect_body_function_compare(const XrXirCompileContext *work,
    const EffectBodyFunction *body,const XrXirFunction *source,bool *same) {
    EffectBodyRead read=(EffectBodyRead){.owner=work};

    if ((source->name_length && !source->name) || (source->parameter_count && !source->parameters) ||
        (source->block_count && !source->blocks) || (source->instruction_count && !source->instructions) ||
        (source->operand_count && !source->operands) || (body->name_length && !body->name) ||
        (body->parameter_count && !body->parameters) || (body->block_count && !body->blocks) ||
        (body->operand_count && !body->operands) || (source->consume_parameter_count && !source->consume_parameters) ||
        (body->consume_parameter_count && !body->consume_parameters)) return XR_XIR_BAD_STRUCTURE;
    if (source->name_length!=body->name_length || source->parameter_count!=body->parameter_count ||
        source->result!=body->result || source->block_count!=body->block_count ||
        source->instruction_count!=body->instruction_count || source->operand_count!=body->operand_count ||
        source->error_type!=body->error_type || source->consume_parameter_count!=body->consume_parameter_count)
        return effect_body_difference(same);
    const void *left[5]={source->name,source->parameters,source->blocks,source->operands,source->consume_parameters};
    const void *right[5]={body->name,body->parameters,body->blocks,body->operands,body->consume_parameters};
    uint64_t bytes[5]={source->name_length,(uint64_t)source->parameter_count*sizeof(*source->parameters),
        (uint64_t)source->block_count*sizeof(*source->blocks),(uint64_t)source->operand_count*sizeof(*source->operands),
        (uint64_t)source->consume_parameter_count*sizeof(*source->consume_parameters)};
    for (uint32_t p=0;p<5;++p) {
        if (bytes[p]>SIZE_MAX) return XR_XIR_BUDGET;

        if (bytes[p] && memcmp(left[p],right[p],(size_t)bytes[p])) {
            XrXirStatus difference=effect_body_difference(same);
            if (difference!=XR_XIR_OK) return difference;
        }
    }
    return effect_body_records_compare(&read,body,source,same);
}

static XrXirStatus effect_body_function_match(const XrXirCompileContext *work,
    const EffectBodyFunction *body,const XrXirFunction *source) {
    return effect_body_function_compare(work,body,source,NULL);
}

#include "xxir_effect_snapshot_continuation.inc.c"

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
        status=effect_body_type_nodes_match(&body->terms.types,types);
    effect_terms_free(&terms);
    return status;
}

/* A Source receiver requires the exact ordered declaration and node fields
 * retained by the sole type clone. Reinterpreting those same numeric children
 * through a temporary substitution domain constructs no additional evidence.
 * Lowered identity projection still uses its independent semantic matcher. */
static XrXirStatus effect_body_source_types_compare(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,const XrXirModule *source,bool checked_copy) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || !source ||
        body->resources!=work->resources ||
        (checked_copy?(body->stage!=XR_XIR_BUILT || source->stage!=XR_XIR_CHECKED):source->stage!=body->stage) ||
        source->linkage_kind!=body->linkage_kind ||
        source->function_count!=effect_body_count(body) ||
        (source->function_count && (!source->functions || !body->functions)))
        return XR_XIR_BAD_STRUCTURE;
    if (source->stage==XR_XIR_LOWERED) return effect_body_type_domain_match(work,body,source);
    const XrXirTypes *types=source->types ? source->types : &effect_empty_types;
    XrXirStatus status=effect_body_type_nodes_match(body->domain.types,types);
    if (status==XR_XIR_OK) status=effect_body_type_tables_compare(body->domain.types,types,NULL);
    return status;
}

static XrXirStatus effect_body_source_types_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,const XrXirModule *source) {
    return effect_body_source_types_compare(work,body,source,false);
}

/* Error/source fresh correspondence retains the complete independent body
 * and metadata traversal. Both lifetime adapters use the same sole reader;
 * no pointer identity substitutes for any source field comparison. */

/* Built admission checks the original and its Checked copy against one input.
 * Template evidence is preserved by that copy; a closed instance is not this
 * transition. Every physical field and metadata row still matches below. */

/* The canonical empty pool has no descriptor owner to validate. All
 * nonempty actual descriptors and their nominal/interface graphs are replayed
 * from the sole owned domain, independently of its membership index. */

/* A run is only a lossless storage range. This classifier derives each mask
 * from its actual opcode and type, independent of the stored membership. */
typedef struct EffectBodyPhysicalReplay {
    uint32_t at,rows,origins,classes;
} EffectBodyPhysicalReplay;

/* Every canonical range is read afresh. Only mathematically constant op/type
 * can share a classification; a nonempty mask still compares every ordinal.
 * All other physical fields stay lossless and their consumers replay them. */

/* The index cannot prove its own completeness. Parameters and all complete
 * runs are independently classified from the current sole owned input. */

/* Current intake and callback projections retain exact instructions, types and metadata. */
static XrXirStatus effect_body_current_input_match(const XrXirCompileContext *work,
    const XrXirModule *module,const EffectBodyMaterial *material) {
    XrXirStatus status=effect_body_source_types_match(work,material,module);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_body_function_match(work,&material->functions[f],&module->functions[f]);
    if (status==XR_XIR_OK) status=effect_body_metadata_match(&material->domain,module);
    return status;
}
