/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_lexical.inc.c - Receive lexical numeric directories into each Artifact
 */
#include "xxir_instruction_flow.h"
static XrXirStatus lexical_vector_shape(const XrXirCompileContext *context,
    const void *data, uint32_t count, size_t element) {
    if (!!data != !!count) return XR_XIR_BAD_STRUCTURE;
    size_t elements = count;
    if (elements > SIZE_MAX / element) return XR_XIR_BUDGET;
    return count ? xir_compile_resource_status(xr_compile_resources_admit(
        context->resources, (size_t)count * element)) : XR_XIR_OK;
}

XR_FUNC void xir_lexical_free(XirLexicalProgram *program) {
    if (!program) return;
    for (uint32_t f = 0; program->functions && f < program->function_count; ++f) {
        xr_compile_resources_free((void *)program->functions[f].scopes);
        xr_compile_resources_free((void *)program->functions[f].bindings);
        xr_compile_resources_free((void *)program->functions[f].events);
    }
    xr_compile_resources_free((void *)program->functions);
    *program = (XirLexicalProgram){0};
}

XR_FUNC XrXirStatus xir_lexical_copy(const XrXirCompileContext *context,
    const XirLexicalProgram *source, XirLexicalProgram *output,
    XrCompileRequestControl *control) {
    if (!context || !source || !output || output->functions || output->function_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = lexical_vector_shape(context, source->functions,
        source->function_count, sizeof(*source->functions));
    if (status != XR_XIR_OK || !source->function_count) return status;
    XirLexicalProgram copy = {0};
    XirLexicalFunction *functions = xir_type_copy_zero(context, source->function_count,
        sizeof(*functions), &status, control);
    if (!functions) return status;
    copy.functions = functions;
    copy.function_count = source->function_count;
    for (uint32_t f = 0; f < source->function_count; ++f) {
        const XirLexicalFunction row = source->functions[f];
        XirLexicalFunction *to = &functions[f];
        status = lexical_vector_shape(context, row.scopes, row.scope_count, sizeof(*row.scopes));
        if (status == XR_XIR_OK) status = lexical_vector_shape(context, row.bindings,
            row.binding_count, sizeof(*row.bindings));
        if (status == XR_XIR_OK) status = lexical_vector_shape(context, row.events,
            row.event_count, sizeof(*row.events));
        if (status != XR_XIR_OK) goto failed;
        to->scopes = lower_copy_bytes(context, row.scopes,
            (size_t)row.scope_count * sizeof(*row.scopes), &status, control);
        to->bindings = lower_copy_bytes(context, row.bindings,
            (size_t)row.binding_count * sizeof(*row.bindings), &status, control);
        to->events = lower_copy_bytes(context, row.events,
            (size_t)row.event_count * sizeof(*row.events), &status, control);
        if (status != XR_XIR_OK) goto failed;
        to->scope_count = row.scope_count;
        to->binding_count = row.binding_count;
        to->event_count = row.event_count;
    }
    *output = copy;
    return XR_XIR_OK;
failed:
    xir_lexical_free(&copy);
    return status;
}

static bool lexical_value_type(const XrXirFunction *function, uint32_t value,
    XrXirType *output) {
    if (value < function->parameter_count) {
        if (!function->parameters) return false;
        *output = function->parameters[value];
        return true;
    }
    value -= function->parameter_count;
    if (value >= function->instruction_count || !function->instructions) return false;
    *output = function->instructions[value].type;
    return true;
}

static bool lexical_binding_coordinates_equal(const XirLexicalBinding *a,
    const XirLexicalBinding *b, uint32_t function) {
    return a->key.module==b->key.module && a->key.declaration==b->key.declaration &&
        a->key.node==b->key.node && a->key.parent_declaration==b->key.parent_declaration &&
        b->key.function==function && a->key.storage==b->key.storage &&
        a->scope==b->scope && a->order==b->order && a->value==b->value &&
        a->mode==b->mode && a->role==b->role && a->rebindable==b->rebindable &&
        a->direct_borrowed==b->direct_borrowed;
}

static bool lexical_event_equal(const XirLexicalEvent *a, const XirLexicalEvent *b) {
    return a->block==b->block && a->instruction==b->instruction && a->ordinal==b->ordinal &&
        a->scope==b->scope && a->target==b->target && a->binding==b->binding && a->value==b->value &&
        a->frontier==b->frontier && a->target_frontier==b->target_frontier &&
        a->statement==b->statement && a->parent_statement==b->parent_statement &&
        a->position==b->position && a->kind==b->kind;
}

static XrXirStatus lexical_function_coordinates_match(const XirLexicalFunction *a,
    const XirLexicalFunction *b, uint32_t function, XrCompileRequestControl *control) {
    if (a->scope_count!=b->scope_count || a->binding_count!=b->binding_count ||
        a->event_count!=b->event_count || !!a->scopes!=!!a->scope_count ||
        !!b->scopes!=!!b->scope_count || !!a->bindings!=!!a->binding_count ||
        !!b->bindings!=!!b->binding_count || !!a->events!=!!a->event_count ||
        !!b->events!=!!b->event_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t progress=0;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t s=0;s<a->scope_count;++s) {
        status=xir_receive_step(control,&progress);
        if (status!=XR_XIR_OK) return status;
        if (a->scopes[s].parent!=b->scopes[s].parent || a->scopes[s].node!=b->scopes[s].node)
            return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t v=0;v<a->binding_count;++v) {
        status=xir_receive_step(control,&progress);
        if (status!=XR_XIR_OK) return status;
        if (!lexical_binding_coordinates_equal(&a->bindings[v],&b->bindings[v],function))
            return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t e=0;e<a->event_count;++e) {
        status=xir_receive_step(control,&progress);
        if (status!=XR_XIR_OK) return status;
        if (!lexical_event_equal(&a->events[e],&b->events[e])) return XR_XIR_BAD_STRUCTURE;
    }
    return status;
}

static bool lexical_binding_valid(const XrXirModule *module, uint32_t function,
    const XirLexicalFunction *directory, uint32_t index) {
    const XirLexicalBinding *binding = &directory->bindings[index];
    const XrXirFunction *body = &module->functions[function];
    XrXirType physical = XR_XIR_UNIT;
    if (binding->scope >= directory->scope_count || binding->order != index ||
        binding->key.function != function || (uint32_t)binding->mode > XR_PARAM_MOVE ||
        (uint32_t)binding->role > XIR_LEXICAL_CAPTURE) return false;
    if (binding->value == UINT32_MAX) {
        if (binding->physical != XR_XIR_UNIT) return false;
    } else if (!lexical_value_type(body, binding->value, &physical) || physical != binding->physical)
        return false;
    if (binding->rebindable && !binding->direct_borrowed) {
        if (!xr_xir_type_is_cell(module->types, physical) ||
            xr_xir_cell_element(module->types, physical) != binding->logical) return false;
    } else if (binding->logical != physical) return false;
    /* An immutable local may bind the same SSA value as its initializer,
     * including an incoming parameter. Declaration role remains independent. */
    if (binding->role == XIR_LEXICAL_LOCAL) return true;
    if (binding->value >= body->parameter_count) return false;
    uint32_t captures = module->declarations && module->declarations->functions ?
        module->declarations->functions[function].capture_count : 0;
    if (binding->role == XIR_LEXICAL_CAPTURE) return binding->value < captures;
    if (binding->value < captures) return false;
    uint32_t mode = xr_xir_declared_parameter_mode(module->types, physical,
        body->consume_parameters, body->consume_parameter_count, binding->value);
    if ((uint32_t)binding->mode != mode) return false;
    return binding->role == XIR_LEXICAL_PARAMETER_READ ? binding->mode == XR_PARAM_READ :
        binding->role == XIR_LEXICAL_PARAMETER_REF ? binding->mode == XR_PARAM_REF :
        binding->mode == XR_PARAM_MOVE;
}

typedef struct LexicalOutcome {
    const XrXirFunction *function;
    const XirLexicalEvent *event;
    bool found;
} LexicalOutcome;

static XrXirStatus lexical_outcome_edge(void *opaque,const XirInstructionFlowEdge *edge) {
    LexicalOutcome *outcome=opaque;
    const XirLexicalEvent *event=outcome->event;
    uint32_t frontier=edge->target_block==UINT32_MAX ? 0 :
        outcome->function->blocks[edge->target_block].frontier;
    if (edge->ordinal==event->ordinal && frontier==event->target_frontier &&
        (edge->target_block==UINT32_MAX)==(event->target==UINT32_MAX))
        outcome->found=true;
    return XR_XIR_OK;
}

static bool lexical_event_outcome(const XrXirFunction *function,const XirLexicalEvent *event) {
    if (!function->instructions) return false;
    const XrXirInstruction *op=&function->instructions[event->instruction];
    bool panic=event->position==XIR_LEXICAL_PANIC_EDGE;
    bool invoke=op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT || op->op==XR_XIR_INVOKE_DEFAULT;
    if (event->position==XIR_LEXICAL_ERROR_EDGE &&
        (invoke ? event->ordinal!=1 :
         (op->op!=XR_XIR_CLEANUP_ERROR && op->op!=XR_XIR_JUMP && op->op!=XR_XIR_THROW) ||
            event->ordinal!=0)) return false;
    if (event->position==XIR_LEXICAL_NORMAL_EDGE && invoke && event->ordinal!=0) return false;
    if (panic ? event->kind!=XIR_LEXICAL_PANIC :
        event->position==XIR_LEXICAL_ERROR_EDGE ? event->kind!=XIR_LEXICAL_THROW :
        event->kind!=XIR_LEXICAL_LEAVE && event->kind!=XIR_LEXICAL_BREAK &&
            event->kind!=XIR_LEXICAL_CONTINUE) return false;
    LexicalOutcome outcome={function,event,false};
    XrXirStatus status=xr_xir_instruction_flow_visit(function,event->instruction,event->block,
        panic ? XIR_INSTRUCTION_FLOW_PANIC : XIR_INSTRUCTION_FLOW_TARGET,lexical_outcome_edge,&outcome);
    if (status!=XR_XIR_OK) return false;
    XirInstructionFlowPoint point={function,op,event->instruction,event->block};
    if (panic && xr_xir_instruction_cleanup_exit(op->op)) {
        status=xr_xir_cleanup_panic_edges(&point,true,lexical_outcome_edge,&outcome);
        if (status!=XR_XIR_OK) return false;
    } else if (panic && !function->blocks[event->block].panic &&
        xr_xir_instruction_has_panic_port(op->op) && !event->ordinal && !event->target_frontier &&
        event->target==UINT32_MAX) outcome.found=true;
    if (!panic && op->op==XR_XIR_THROW && event->position==XIR_LEXICAL_ERROR_EDGE &&
        !event->ordinal && !event->target_frontier && event->target==UINT32_MAX) outcome.found=true;
    return outcome.found;
}

static bool lexical_event_valid(const XrXirFunction *function,
    const XirLexicalFunction *directory, const XirLexicalEvent *event) {
    if (event->scope >= directory->scope_count ||
        (event->target != UINT32_MAX && event->target >= directory->scope_count) ||
        (event->binding != UINT32_MAX && event->binding >= directory->binding_count) ||
        event->block >= function->block_count || !function->blocks ||
        (uint32_t)event->position > XIR_LEXICAL_PANIC_EDGE ||
        (uint32_t)event->kind > XIR_LEXICAL_TEMP_END || event->frontier > function->instruction_count ||
        event->target_frontier > function->instruction_count) return false;
    const XrXirBlock *block = &function->blocks[event->block];
    if (event->instruction < block->first || event->instruction - block->first >= block->count)
        return false;
    if (event->position == XIR_LEXICAL_BLOCK_ENTRY && event->instruction != block->first)
        return false;
    if (event->position < XIR_LEXICAL_NORMAL_EDGE && event->ordinal) return false;
    if (event->frontier!=block->frontier || !function->instructions) return false;
    if ((event->frontier && function->instructions[event->frontier-1].op!=XR_XIR_CLEANUP_REGISTER) ||
        (event->target_frontier && function->instructions[event->target_frontier-1].op!=XR_XIR_CLEANUP_REGISTER))
        return false;
    if (event->position>=XIR_LEXICAL_NORMAL_EDGE && !lexical_event_outcome(function,event)) return false;
    XrXirType ignored;
    if (event->value != UINT32_MAX && !lexical_value_type(function, event->value, &ignored)) return false;
    if (event->value!=UINT32_MAX && event->value>=function->parameter_count) {
        uint32_t definition=event->value-function->parameter_count;
        if (definition>event->instruction ||
            (definition==event->instruction && event->position!=XIR_LEXICAL_AFTER)) return false;
    }
    if ((event->kind == XIR_LEXICAL_BIND || event->kind == XIR_LEXICAL_REPLACE ||
        event->kind == XIR_LEXICAL_TAKE) && event->binding == UINT32_MAX) return false;
    if ((event->kind == XIR_LEXICAL_TEMP_BEGIN || event->kind == XIR_LEXICAL_TEMP_END) &&
        (!event->statement || event->statement==event->parent_statement)) return false;
    if (event->kind==XIR_LEXICAL_ENTER &&
        directory->scopes[event->scope].parent!=event->target) return false;
    if (event->kind!=XIR_LEXICAL_ENTER && event->target!=UINT32_MAX) {
        uint32_t scope=event->scope;
        while (scope!=event->target && scope!=UINT32_MAX) scope=directory->scopes[scope].parent;
        if (scope==UINT32_MAX) return false;
    }
    if ((event->kind==XIR_LEXICAL_BIND || event->kind==XIR_LEXICAL_REPLACE ||
        event->kind==XIR_LEXICAL_TAKE) &&
        directory->bindings[event->binding].scope!=event->target) return false;
    if (event->kind==XIR_LEXICAL_BIND &&
        directory->bindings[event->binding].value!=event->value) return false;
    const XrXirInstruction *op=&function->instructions[event->instruction];
    if (event->kind==XIR_LEXICAL_RETURN &&
        (event->position>=XIR_LEXICAL_NORMAL_EDGE || op->op!=XR_XIR_RETURN ||
         event->value!=(function->result==XR_XIR_UNIT ? UINT32_MAX : op->args[0]))) return false;
    if ((event->kind==XIR_LEXICAL_BREAK || event->kind==XIR_LEXICAL_CONTINUE) &&
        op->op!=XR_XIR_JUMP && op->op!=XR_XIR_CLEANUP_LEAVE) return false;
    return true;
}

XR_FUNC XrXirStatus xir_lexical_verify(const XrXirCompileContext *context,
    const XrXirModule *module, const XirLexicalProgram *program) {
    if (!context || !module || !program ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = lexical_vector_shape(context, program->functions,
        program->function_count, sizeof(*program->functions));
    if (status != XR_XIR_OK || !program->function_count) return status;
    if (program->function_count != module->function_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < program->function_count; ++f) {
        const XirLexicalFunction *row = &program->functions[f];
        status = lexical_vector_shape(context, row->scopes, row->scope_count, sizeof(*row->scopes));
        if (status == XR_XIR_OK) status = lexical_vector_shape(context, row->bindings,
            row->binding_count, sizeof(*row->bindings));
        if (status == XR_XIR_OK) status = lexical_vector_shape(context, row->events,
            row->event_count, sizeof(*row->events));
        if (status != XR_XIR_OK) return status;
        if (!row->scope_count && (row->binding_count || row->event_count)) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t s = 0; s < row->scope_count; ++s)
            if (s ? row->scopes[s].parent >= s : row->scopes[s].parent != UINT32_MAX)
                return XR_XIR_BAD_STRUCTURE;
        for (uint32_t b = 0; b < row->binding_count; ++b)
            if (!lexical_binding_valid(module, f, row, b)) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t e = 0; e < row->event_count; ++e)
            if (!lexical_event_valid(&module->functions[f], row, &row->events[e]))
                return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_xir_compile_lexical_function(const XrXirArtifact *artifact,
    uint32_t function, XirLexicalFunction *output) {
    if (!artifact || !output || function >= artifact->module.function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (artifact->module.stage < XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    const XrXirProvenance *provenance = artifact->module.provenance;
    if (!provenance || !provenance->lexical.functions ||
        provenance->lexical.function_count != artifact->module.function_count ||
        !provenance->lexical.functions[function].scope_count) return XR_XIR_UNRESOLVED;
    *output = provenance->lexical.functions[function];
    return XR_XIR_OK;
}
