/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_provenance_match.inc.c - Template to instance function correspondence
 *
 * KEY CONCEPT:
 *   Independent consumers can check structural substitution without a producer arena.
 */
#include <stdio.h>
#include "xxir_constraints.h"
#include "xxir_type_match_internal.h"
#include "xxir_defaults_internal.h"
#include "xxir_constraint_proof.h"
#include "xxir_implementation_verify.h"
#include "xxir_interface_members.h"
typedef struct ProvenanceMatch {
    const XrXirModule *source, *destination;
    const XrXirOrigin *origins;
    uint32_t count;
    XrXirCompileContext *remaining;
    XrXirDiagnostic *diagnostic;
    XrXirTypeMatchScratch scratch;
} ProvenanceMatch;
static XrXirStatus provenance_call_match(ProvenanceMatch *c, const XrXirOrigin *origin,
    const XrXirInstruction *from, const XrXirInstruction *to) {
    if (to->immediate < 0 || (uint64_t)to->immediate >= c->count || to->type_arguments[0] || to->type_arguments[1])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirOrigin *target = &c->origins[to->immediate];
    if (target->function != (uint32_t)from->immediate || target->argument_count != from->type_arguments[1])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirGeneric *generic = c->source->generics ? &c->source->generics[origin->function] : NULL;
    uint32_t arguments = generic ? generic->argument_count : 0;
    if (from->type_arguments[0] > arguments || target->argument_count > arguments - from->type_arguments[0])
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < target->argument_count; ++a) {
        XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, c->destination->types, origin->arguments, origin->argument_count, generic->arguments[from->type_arguments[0] + a], target->arguments[a], &c->scratch);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus provenance_default_match(ProvenanceMatch *c,const XrXirOrigin *origin,
    const XrXirInstruction *from,const XrXirInstruction *to) {
    if(to->args[0] || to->args[1]) return XR_XIR_BAD_STRUCTURE;
    if(from->op==XR_XIR_CALL_DEFAULT) {
        if(to->op!=XR_XIR_CALL || to->targets[0] || to->targets[1]) return XR_XIR_BAD_STRUCTURE;
    } else if(from->op!=XR_XIR_INVOKE_DEFAULT || to->op!=XR_XIR_INVOKE ||
        from->targets[0]!=to->targets[0] || from->targets[1]!=to->targets[1]) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_xir_compile_default_call_verify(c->remaining, c->source, origin->function, from);
    if(status!=XR_XIR_OK) return status;
    const XrXirDefaultBinding *binding=NULL;
    const uint32_t *identity=xr_xir_default_identity(from);
    status=xr_xir_compile_default_lookup(c->remaining, c->source, identity[0], identity[1], &binding);
    if(status!=XR_XIR_OK) return status;
    if(!binding) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction ordinary=*from; ordinary.immediate=binding->function;
    return provenance_call_match(c,origin,&ordinary,to);
}
static XrXirStatus provenance_requirement_match(ProvenanceMatch *c, const XrXirOrigin *origin,
    const XrXirInstruction *from, const XrXirInstruction *to, const XrXirFunction *function) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (to->op != XR_XIR_CALL || to->targets[0] || to->targets[1] || to->type_arguments[0] ||
        to->type_arguments[1] || to->immediate < 0 || (uint64_t)to->immediate >= c->count ||
        !to->args[1] || to->args[0] >= function->operand_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirGeneric *generic = c->source->generics ? &c->source->generics[origin->function] : NULL;
    uint32_t count = generic ? generic->argument_count : 0;
    if (from->type_arguments[0] > count || from->type_arguments[1] > count - from->type_arguments[0])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceTable *interfaces = c->source->types ? c->source->types->interfaces : NULL;
    if (!interfaces || from->targets[0] >= interfaces->count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *interface = &interfaces->declarations[from->targets[0]];
    if (from->targets[1] >= interface->method_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceMethod *method = &interface->methods[from->targets[1]];
    uint32_t parent = interface->parameter_count, own = method->own_parameter_count;
    if (parent > 65536 || own > 65536 - parent || from->type_arguments[1] != parent+own)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInterfaceApplication root = {from->targets[0],parent ?
        generic->arguments + from->type_arguments[0] : NULL,parent};
    XrXirInterfaceClosureRequest substitution = {c->source->types,c->destination->types,
        &root,1,origin->arguments,origin->argument_count,0};
    XrXirInterfaceClosure *closure = NULL;
    XrXirStatus status = xr_xir_compile_interface_closure_substitute(c->remaining, &substitution, &closure);
    if (status != XR_XIR_OK) {  return status; }
    const XrXirInterfaceApplication *application = NULL;
    for (uint32_t a = 0; a < xr_xir_interface_closure_application_count(closure); ++a) {
        if (!xir_compile_work(c->remaining, 1)) { status = XR_XIR_BUDGET; break; }

        const XrXirInterfaceApplication *candidate = xr_xir_interface_closure_application(closure,a);
        if (candidate->declaration == root.declaration) { application = candidate; break; }
    }
    if (status == XR_XIR_OK && !application) status = XR_XIR_BAD_STRUCTURE;
    if (status == XR_XIR_OK) {
        XrXirModule actual = *c->destination; actual.types = xr_xir_interface_closure_types(closure);
        XrXirProofContext context = {&actual,{XR_XIR_CONTEXT_CLOSED,0,0}};
        XrXirWitnessRequest request = {c->source,
            xr_xir_operand_type(function,function->operands[to->args[0]]),*application,from->targets[1]};
        XrXirWitness witness = {0};
        status = xr_xir_compile_witness_resolve(c->remaining, &context, &request, &witness);
        const XrXirOrigin *target = &c->origins[to->immediate];
        if (status == XR_XIR_OK && (witness.argument_count > 65536 || own > 65536-witness.argument_count ||
            target->function != witness.function || target->argument_count != witness.argument_count+own))
            status = XR_XIR_BAD_STRUCTURE;
        for (uint32_t a = 0; a < witness.argument_count && status == XR_XIR_OK; ++a)
            status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, actual.types, c->destination->types, NULL, 0, witness.arguments[a], target->arguments[a], &c->scratch);
        for (uint32_t a = 0; a < own && status == XR_XIR_OK; ++a)
            status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, c->destination->types, origin->arguments, origin->argument_count, generic->arguments[from->type_arguments[0]+parent+a], target->arguments[witness.argument_count+a], &c->scratch);
        if (status == XR_XIR_OK && own) {
            uint32_t total = parent+own;
            uint64_t bytes = (uint64_t)total*sizeof(XrXirType);
            if (bytes > SIZE_MAX ||!xir_compile_work(c->remaining, total))
                status = XR_XIR_BUDGET;
            else {

                XrXirType *arguments = xir_compile_alloc(c->remaining, (size_t)bytes, &allocation_status);
                if (!arguments) status = allocation_status;
                else {
                    if (!xir_compile_work(c->remaining, (uint64_t)(parent + own) * sizeof(*arguments))) {
        xr_compile_resources_free(arguments); xr_xir_compile_interface_closure_free(closure); return XR_XIR_BUDGET;
    }
    if (parent) memcpy(arguments,application->arguments,parent*sizeof(*arguments));
                    memcpy(arguments+parent,target->arguments+witness.argument_count,own*sizeof(*arguments));
                    for (uint32_t a = 0; a < own && status == XR_XIR_OK; ++a) {
                        XrXirConstraintUse use = {c->source,
                            {XR_XIR_CONTEXT_INTERFACE_METHOD,root.declaration,from->targets[1]},parent+a,arguments,total};
                        status = xr_xir_compile_constraints_prove(c->remaining, &context, &use);
                    }
                    xr_compile_resources_free(arguments);
                }

            }
        }
    }
    xr_xir_compile_interface_closure_free(closure);
     return status;
}
static XrXirStatus provenance_name(ProvenanceMatch *c, const XrXirOrigin *origin,
    const XrXirFunction *from, const XrXirFunction *to) {
    if (!to->name || to->name_length < from->name_length) return XR_XIR_BAD_STRUCTURE;
    uint64_t work = (uint64_t)to->name_length + origin->argument_count;
    if (!xir_compile_work(c->remaining, work)) return XR_XIR_BUDGET;

    if (memcmp(from->name, to->name, from->name_length)) return XR_XIR_BAD_STRUCTURE;
    size_t at = from->name_length;
    if (origin->argument_count) {
        for (uint32_t i = 0; i <= origin->argument_count; ++i) {
            char part[12];
            int n = snprintf(part, sizeof(part), i ? ":%u" : "$%u",
                (unsigned)(i ? origin->arguments[i - 1] : origin->function));
            if (n < 0 || (size_t)n >= sizeof(part) || (size_t)n > to->name_length - at ||
                memcmp(to->name + at, part, (size_t)n)) return XR_XIR_BAD_STRUCTURE;
            at += (size_t)n;
        }
    }
    return at == to->name_length ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}

static XrXirOp provenance_lowered_op(const XrXirTypes *types, const XrXirFunction *function,
    XrXirOp source, const XrXirInstruction *output) {
    if (source == XR_XIR_COPY)
        return xr_xir_type_is_owned(types, output->type) ? XR_XIR_OWNED_RETAIN : XR_XIR_SCALAR_COPY;
    if (source >= XR_XIR_LOCAL_NEW && source <= XR_XIR_LOCAL_WRITE) {
        XrXirType type = output->type;
        if (source == XR_XIR_LOCAL_WRITE) {
            uint32_t receiver = output->args[0];
            if (receiver < function->parameter_count || receiver - function->parameter_count >= function->instruction_count)
                return XR_XIR_OP_COUNT;
            type = function->instructions[receiver - function->parameter_count].type;
        }
        return (XrXirOp)((xr_xir_type_is_owned(types, type) ? XR_XIR_OWNED_LOCAL_NEW : XR_XIR_SCALAR_LOCAL_NEW) +
            source - XR_XIR_LOCAL_NEW);
    }
    return source;
}

static XrXirStatus provenance_functions_match(ProvenanceMatch *c) {
    for (uint32_t f = 0; f < c->count; ++f) {
        const XrXirOrigin *origin = &c->origins[f];
        if (origin->function >= c->source->function_count ||
            origin->argument_count != (c->source->generics ? c->source->generics[origin->function].parameter_count : 0))
            return XR_XIR_BAD_STRUCTURE;
        XrXirProofContext environment = {c->destination, {XR_XIR_CONTEXT_CLOSED,0,0}};
        for (uint32_t a = 0; a < origin->argument_count; ++a) {
            XrXirConstraintUse use = {c->source, {XR_XIR_CONTEXT_FUNCTION,origin->function,0},
                a, origin->arguments, origin->argument_count};
            bool result = xr_xir_binder_kind(&c->source->generics[origin->function],a) == XR_XIR_BINDER_RESULT_VARIABLE;
            /* Closed substitutions prove the storage domain. Source callers
             * already proved visibility; a callee's template scope must not
             * acquire or reject private concrete declaration access. */
            XrXirStatus status = result && origin->arguments[a] == XR_XIR_UNIT ? XR_XIR_OK :
                xr_xir_compile_type_constraints(c->remaining, c->destination, f, origin->arguments[a], (XrXirConstraint){0});
            if (status == XR_XIR_OK) status = xr_xir_compile_constraints_prove(c->remaining, &environment, &use);
            if (status != XR_XIR_OK) return status;
        }
        const XrXirFunction *from = &c->source->functions[origin->function], *to = &c->destination->functions[f];
        c->diagnostic->function = origin->function;
        XrXirStatus name_status = provenance_name(c, origin, from, to);
        if (name_status != XR_XIR_OK) return name_status;
        if (from->parameter_count != to->parameter_count || from->instruction_count != to->instruction_count ||
            from->block_count != to->block_count || from->operand_count != to->operand_count ||
            (to->parameter_count && !to->parameters) || !to->blocks || !to->instructions ||
            (to->operand_count && !to->operands))
            return XR_XIR_BAD_STRUCTURE;
        if (c->source->declarations) {
            const XrXirFunctionIdentity *identity = &c->source->declarations->functions[origin->function];
            const XrXirFunctionIdentity *actual = &c->destination->declarations->functions[f];
            if (identity->module != actual->module || identity->exported != actual->exported ||
                identity->nominal_owner != actual->nominal_owner || identity->member_access != actual->member_access ||
                identity->promises != actual->promises || identity->method_kind != actual->method_kind ||
                identity->test_role != actual->test_role || identity->test_timeout_seconds != actual->test_timeout_seconds)
                return XR_XIR_BAD_STRUCTURE;
            if (!!identity->cleanup_owner != !!actual->cleanup_owner) return XR_XIR_BAD_STRUCTURE;
            if (identity->cleanup_owner) {
                if (actual->cleanup_owner > f) return XR_XIR_BAD_STRUCTURE;
                const XrXirOrigin *parent = &c->origins[actual->cleanup_owner - 1];
                if (parent->function != identity->cleanup_owner - 1 ||
                    parent->argument_count != origin->argument_count) return XR_XIR_BAD_STRUCTURE;
                for (uint32_t a = 0; a < origin->argument_count; ++a) {
                    if (!xir_compile_work(c->remaining, 1)) return XR_XIR_BUDGET;

                    if (parent->arguments[a] != origin->arguments[a]) return XR_XIR_BAD_TYPE;
                }
            }
        }
        uint64_t work = (uint64_t)from->block_count + from->operand_count + from->instruction_count;
        if (!xir_compile_work(c->remaining, work)) return XR_XIR_BUDGET;

        for (uint32_t b = 0; b < from->block_count; ++b)
            if (from->blocks[b].first != to->blocks[b].first || from->blocks[b].count != to->blocks[b].count ||
                from->blocks[b].panic != to->blocks[b].panic || from->blocks[b].frontier != to->blocks[b].frontier)
                return XR_XIR_BAD_STRUCTURE;
        for (uint32_t o = 0; o < from->operand_count; ++o)
            if (from->operands[o] != to->operands[o]) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t i = 0; i < from->instruction_count; ++i) {
            const XrXirInstruction *a = &from->instructions[i], *b = &to->instructions[i];
            c->diagnostic->instruction = i;
            XrXirOp expected = (a->op == XR_XIR_CALL_REQUIREMENT || a->op == XR_XIR_CALL_DEFAULT) ? XR_XIR_CALL : a->op == XR_XIR_INVOKE_DEFAULT ? XR_XIR_INVOKE : c->destination->stage == XR_XIR_LOWERED ?
                provenance_lowered_op(c->destination->types, to, a->op, b) : a->op;
            if (expected != b->op || (a->op != XR_XIR_INVOKE_DEFAULT &&
                    (a->args[0] != b->args[0] || a->args[1] != b->args[1])) ||
                (a->op != XR_XIR_CALL_REQUIREMENT && a->op != XR_XIR_CALL_DEFAULT &&
                    (a->targets[0] != b->targets[0] || a->targets[1] != b->targets[1]))) return XR_XIR_BAD_STRUCTURE;
            if (a->op == XR_XIR_CALL_DEFAULT || a->op == XR_XIR_INVOKE_DEFAULT) {
                XrXirStatus status=provenance_default_match(c,origin,a,b);
                if(status!=XR_XIR_OK) return status;
            } else if (a->op == XR_XIR_CALL_REQUIREMENT) {
                XrXirStatus status = provenance_requirement_match(c,origin,a,b,to);
                if (status != XR_XIR_OK) return status;
            } else if (xr_xir_op_references_function(a->op)) {
                XrXirStatus status = provenance_call_match(c, origin, a, b);
                if (status != XR_XIR_OK) return status;
            } else if (a->op == XR_XIR_ERROR_IS) {
                if (a->type_arguments[0] || a->type_arguments[1] || b->type_arguments[0] || b->type_arguments[1] ||
                    a->immediate < 0 || b->immediate < 0 || (uint64_t) a->immediate > UINT32_MAX ||
                    (uint64_t) b->immediate > UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
                XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, c->destination->types, origin->arguments, origin->argument_count, (XrXirType) a->immediate, (XrXirType) b->immediate, &c->scratch);
                if (status != XR_XIR_OK) return status;
            } else if (a->immediate != b->immediate || a->type_arguments[0] != b->type_arguments[0] || a->type_arguments[1] != b->type_arguments[1])
                return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t t = 0; t < 1 + from->parameter_count + from->instruction_count; ++t) {
            XrXirType expected = !t ? from->result : t <= from->parameter_count ? from->parameters[t - 1] :
                from->instructions[t - from->parameter_count - 1].type;
            XrXirType actual = !t ? to->result : t <= to->parameter_count ? to->parameters[t - 1] :
                to->instructions[t - to->parameter_count - 1].type;
            XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, c->destination->types, origin->arguments, origin->argument_count, expected, actual, &c->scratch);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus provenance_bytes(ProvenanceMatch *c, const char *a, uint32_t an,
    const char *b, uint32_t bn) {
    if (an != bn || (an && (!a || !b))) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(c->remaining, an)) return XR_XIR_BUDGET;

    return an && memcmp(a, b, an) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}

static XrXirStatus provenance_nominal_fields(ProvenanceMatch *c,
    const XrXirNominalDeclaration *a, const XrXirNominalDeclaration *b) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint64_t bytes = (uint64_t)a->parameter_count * sizeof(XrXirType);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

    XrXirType *parameters = bytes ? xir_compile_alloc(c->remaining, (size_t)bytes, &allocation_status) : NULL;
    if (bytes && !parameters) return allocation_status;
    for (uint32_t p = 0; p < a->parameter_count; ++p)
        parameters[p] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + p);
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t f = 0; f < a->field_count; ++f) {
        const XrXirNominalField *from = &a->fields[f], *to = &b->fields[f];
        if (from->flags != to->flags) { status = XR_XIR_BAD_STRUCTURE; break; }
        status = provenance_bytes(c, from->name.bytes, from->name.length, to->name.bytes, to->name.length);
        if (status != XR_XIR_OK) break;
        status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, c->destination->types, parameters, a->parameter_count, from->type, to->type, &c->scratch);
        if (status != XR_XIR_OK) break;
    }
    xr_compile_resources_free(parameters);
    return status;
}

static XrXirStatus provenance_variants(ProvenanceMatch *c,
    uint32_t kind, const XrXirNominalVariant *a, uint32_t count,
    uint32_t actual_kind, const XrXirNominalVariant *b, uint32_t actual_count) {
    if (kind != actual_kind || count != actual_count || (count && (!a || !b))) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(c->remaining, count)) return XR_XIR_BUDGET;

    for (uint32_t i = 0; i < count; ++i) {
        if (a[i].field_begin != b[i].field_begin || a[i].field_count != b[i].field_count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = provenance_bytes(c, a[i].name.bytes, a[i].name.length, b[i].name.bytes, b[i].name.length);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus provenance_nominal_instances(ProvenanceMatch *c, const XrXirNominalTable *source) {
    const XrXirTypes *types = c->destination->types;
    if (!xir_compile_work(c->remaining, types->count)) return XR_XIR_BUDGET;

    for (uint32_t n = 0; n < types->count; ++n) {
        const XrXirTypeNode *node = &types->nodes[n];
        if (node->kind != XR_XIR_TYPE_NOMINAL || node->parameter_span) continue;
        if (node->nominal.declaration >= source->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirNominalDeclaration *d = &source->declarations[node->nominal.declaration];
        if (node->nominal.argument_count != d->parameter_count || node->nominal.field_count != d->field_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirProofContext environment = {c->destination,{XR_XIR_CONTEXT_CLOSED,0,0}};
        for (uint32_t a = 0; a < d->parameter_count; ++a) {
            XrXirConstraintUse use = {c->source,{XR_XIR_CONTEXT_NOMINAL,node->nominal.declaration,0},a,
                node->nominal.arguments,node->nominal.argument_count};
            XrXirStatus status = xr_xir_compile_constraints_prove(c->remaining, &environment, &use);
            if (status != XR_XIR_OK) return status;
        }
        for (uint32_t f = 0; f < d->field_count; ++f) {
            XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, types, node->nominal.arguments, node->nominal.argument_count, d->fields[f].type, node->nominal.fields[f], &c->scratch);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus provenance_lowered_nominals(ProvenanceMatch *c,
    const XrXirNominalTable *source, const XrXirNominalTable *output) {
    if (output->declarations || (output->count && !output->identities)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t d = 0; d < source->count; ++d) {
        const XrXirNominalDeclaration *a = &source->declarations[d];
        const XrXirNominalIdentity *b = &output->identities[d];
        if (a->flags != b->flags) return XR_XIR_BAD_TYPE;
        XrXirStatus variants = provenance_variants(c, a->kind, a->variants, a->variant_count,
            b->kind, b->variants, b->variant_count);
        if (variants != XR_XIR_OK) return variants;
        if (a->exported != b->exported || a->parameter_count != b->arity || a->field_count != b->field_count ||
            (b->field_count && !b->fields)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(c->remaining, (uint64_t)a->field_count + 1)) return XR_XIR_BUDGET;

        XrXirStatus status = provenance_bytes(c, a->module.bytes, a->module.length, b->module.bytes, b->module.length);
        if (status != XR_XIR_OK) return status;
        status = provenance_bytes(c, a->name.bytes, a->name.length, b->name.bytes, b->name.length);
        if (status != XR_XIR_OK) return status;
        for (uint32_t f = 0; f < a->field_count; ++f) {
            if (a->fields[f].flags != b->fields[f].flags) return XR_XIR_BAD_STRUCTURE;
            status = provenance_bytes(c, a->fields[f].name.bytes, a->fields[f].name.length,
                b->fields[f].name.bytes, b->fields[f].name.length);
            if (status != XR_XIR_OK) return status;
        }
    }
    return provenance_nominal_instances(c,source);
}

static XrXirStatus provenance_nominals(ProvenanceMatch *c) {
    const XrXirNominalTable *a = c->source->types ? c->source->types->nominals : NULL;
    const XrXirNominalTable *b = c->destination->types ? c->destination->types->nominals : NULL;
    if (!!a != !!b) return XR_XIR_BAD_STRUCTURE;
    if (!a) return XR_XIR_OK;
    if (a->count != b->count) return XR_XIR_BAD_STRUCTURE;
    if (c->destination->stage == XR_XIR_LOWERED) return provenance_lowered_nominals(c, a, b);
    if (b->identities || (b->count && !b->declarations)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(c->remaining, a->count)) return XR_XIR_BUDGET;

    for (uint32_t d = 0; d < a->count; ++d) {
        const XrXirNominalDeclaration *from = &a->declarations[d], *to = &b->declarations[d];
        if (from->flags != to->flags) return XR_XIR_BAD_TYPE;
        XrXirStatus variants = provenance_variants(c, from->kind, from->variants, from->variant_count,
            to->kind, to->variants, to->variant_count);
        if (variants != XR_XIR_OK) return variants;
        if (from->exported != to->exported || from->parameter_count != to->parameter_count ||
            from->field_count != to->field_count || (to->parameter_count && !to->constraints) ||
            (to->field_count && !to->fields)) return XR_XIR_BAD_STRUCTURE;
        uint64_t work = (uint64_t)from->parameter_count + from->field_count;
        if (!xir_compile_work(c->remaining, work)) return XR_XIR_BUDGET;

        XrXirStatus status = provenance_bytes(c, from->module.bytes, from->module.length,
            to->module.bytes, to->module.length);
        if (status != XR_XIR_OK) return status;
        status = provenance_bytes(c, from->name.bytes, from->name.length, to->name.bytes, to->name.length);
        if (status != XR_XIR_OK) return status;
        for (uint32_t p = 0; p < from->parameter_count; ++p) {
            if (from->constraints[p].markers != to->constraints[p].markers ||
                to->constraints[p].interfaces || to->constraints[p].interface_count) return XR_XIR_BAD_STRUCTURE;
        }
        status = provenance_nominal_fields(c, from, to);
        if (status != XR_XIR_OK) return status;
    }
    return provenance_nominal_instances(c,a);
}

static bool provenance_ordinary(ProvenanceMatch *c, uint32_t source, uint32_t output) {
    return output < c->count && c->origins[output].function == source && !c->origins[output].argument_count;
}

static XrXirStatus provenance_declarations(ProvenanceMatch *c) {
    const XrXirDeclarations *a = c->source->declarations, *b = c->destination->declarations;
    if (!a) return XR_XIR_OK;
    if (b->implementations || (c->destination->types && c->destination->types->interfaces))
        return XR_XIR_BAD_STRUCTURE;
    if (a->module_count != b->module_count || a->literal_count != b->literal_count ||
        a->slot_count != b->slot_count || a->root_module != b->root_module ||
        !b->modules || (b->literal_count && !b->literals) || (b->slot_count && !b->slots) ||
        !provenance_ordinary(c, a->entry_function, b->entry_function)) return XR_XIR_BAD_STRUCTURE;
    uint64_t work = (uint64_t)a->module_count + a->literal_count + a->slot_count;
    if (!xir_compile_work(c->remaining, work)) return XR_XIR_BUDGET;

    for (uint32_t m = 0; m < a->module_count; ++m) {
        const XrXirSourceModule *from = &a->modules[m], *to = &b->modules[m];
        if (from->dependency_count != to->dependency_count ||
            (to->dependency_count && !to->dependencies) ||
            !provenance_ordinary(c, from->initializer, to->initializer)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = provenance_bytes(c, from->name, from->name_length, to->name, to->name_length);
        if (status != XR_XIR_OK) return status;
        if (!xir_compile_work(c->remaining, from->dependency_count)) return XR_XIR_BUDGET;

        for (uint32_t d = 0; d < from->dependency_count; ++d)
            if (from->dependencies[d] != to->dependencies[d]) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t l = 0; l < a->literal_count; ++l) {
        XrXirStatus status = provenance_bytes(c, a->literals[l].bytes, a->literals[l].length,
            b->literals[l].bytes, b->literals[l].length);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t s = 0; s < a->slot_count; ++s) {
        if (a->slots[s].module != b->slots[s].module || a->slots[s].mutable != b->slots[s].mutable)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->source->types, c->destination->types, NULL, 0, a->slots[s].type, b->slots[s].type, &c->scratch);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus provenance_reachable(ProvenanceMatch *c) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint64_t bytes = (uint64_t)c->count * (sizeof(uint32_t) + 1) + c->source->function_count;
    uint64_t work = (uint64_t)c->count + c->source->function_count;
    if (bytes > SIZE_MAX ||!xir_compile_work(c->remaining, work))
        return XR_XIR_BUDGET;


    uint32_t *queue = xir_compile_alloc(c->remaining, (size_t)bytes, &allocation_status);
    if (!queue) return allocation_status;
    uint8_t *seen = (uint8_t *)(queue + c->count), *roots = seen + c->count;
    if (!xir_compile_work(c->remaining, (uint64_t)c->count + c->source->function_count)) {
        xr_compile_resources_free(queue); return XR_XIR_BUDGET;
    }
    memset(seen, 0, (size_t)c->count + c->source->function_count);
    uint32_t tail = 0;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t f = 0; f < c->count; ++f) {
        const XrXirOrigin *origin = &c->origins[f];
        if (!origin->argument_count) {
            if (roots[origin->function]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
            roots[origin->function] = 1; seen[f] = 1; queue[tail++] = f;
        }
        for (uint32_t prior = 0; prior < f; ++prior) {
            if (!xir_compile_work(c->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }

            const XrXirOrigin *other = &c->origins[prior];
            if (origin->function != other->function || origin->argument_count != other->argument_count) continue;
            bool same = true;
            for (uint32_t a = 0; a < origin->argument_count; ++a) {
                if (!xir_compile_work(c->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }

                if (origin->arguments[a] != other->arguments[a]) { same = false; break; }
            }
            if (same) { status = XR_XIR_BAD_STRUCTURE; goto done; }
        }
    }
    bool has_cleanup = false;
    for (uint32_t f = 0; f < c->source->function_count; ++f) {
        if (c->source->declarations && c->source->declarations->functions[f].cleanup_owner) has_cleanup = true;
        if ((!c->source->generics || !c->source->generics[f].parameter_count) && !roots[f]) {
            status = XR_XIR_BAD_STRUCTURE; goto done;
        }
    }
    for (uint32_t head = 0; head < tail; ++head) {
        const XrXirFunction *function = &c->destination->functions[queue[head]];
        if (!xir_compile_work(c->remaining, function->instruction_count)) { status = XR_XIR_BUDGET; goto done; }

        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (!xr_xir_op_references_function(op->op)) continue;
            if (op->immediate < 0 || (uint64_t)op->immediate >= c->count) {
                status = XR_XIR_BAD_STRUCTURE; goto done;
            }
            uint32_t target = (uint32_t)op->immediate;
            if (!seen[target]) { seen[target] = 1; queue[tail++] = target; }
        }
        if (has_cleanup) {
            for (uint32_t child = 0; child < c->source->function_count; ++child) {
                if (!xir_compile_work(c->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }

                if (c->source->declarations->functions[child].cleanup_owner != c->origins[queue[head]].function + 1) continue;
                bool found = false;
                for (uint32_t target = 0; target < c->count; ++target) {
                    if (!xir_compile_work(c->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }

                    if (c->origins[target].function != child ||
                        c->destination->declarations->functions[target].cleanup_owner != queue[head] + 1) continue;
                    if (found) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                    found = true;
                    if (!seen[target]) { seen[target] = 1; queue[tail++] = target; }
                }
                if (!found) { status = XR_XIR_BAD_STRUCTURE; goto done; }
            }
        }
    }
    if (tail != c->count) status = XR_XIR_BAD_STRUCTURE;
done:
    xr_compile_resources_free(queue);
    return status;
}

XrXirStatus xr_xir_compile_provenance_functions_match(const XrXirCompileContext *compile_context, const XrXirModule *source, const XrXirModule *destination, const XrXirOrigin *origins, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    XrXirDiagnostic location = {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status = XR_XIR_OK;
    if (!source || !destination || !origins || !remaining || !source->functions ||
        !destination->functions || !destination->function_count || destination->generics || destination->defaults ||
        (!!source->declarations != !!destination->declarations) ||
        (destination->declarations && !destination->declarations->functions))
        status = XR_XIR_BAD_STRUCTURE;
    else if (source->stage != XR_XIR_CHECKED ||
        (destination->stage != XR_XIR_CHECKED && destination->stage != XR_XIR_LOWERED))
        status = XR_XIR_BAD_STAGE;
    else if (!xir_compile_work(remaining, destination->function_count)) status = XR_XIR_BUDGET;
    else {

        for (uint32_t f = 0; f < destination->function_count; ++f) {
            const XrXirOrigin *origin = &origins[f];
            if (origin->function >= source->function_count ||
                (!!origin->arguments != !!origin->argument_count) ||
                origin->argument_count != (source->generics ? source->generics[origin->function].parameter_count : 0)) {
                status = XR_XIR_BAD_STRUCTURE; break;
            }
        }
        if (status == XR_XIR_OK) {
            ProvenanceMatch context = {source, destination, origins, destination->function_count, remaining, &location,
                {remaining->resources, NULL}};
            status = provenance_nominals(&context);
            if (status == XR_XIR_OK) status = provenance_functions_match(&context);
            if (status == XR_XIR_OK) status = provenance_declarations(&context);
            if (status == XR_XIR_OK) status = provenance_reachable(&context);
            xr_xir_type_match_scratch_free(&context.scratch);
        }
    }
    location.status = status;
    if (diagnostic) *diagnostic = location;
    return status;
}
