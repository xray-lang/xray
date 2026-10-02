/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraint_proof.c - Bounded proofs from explicit declaration facts
 *
 * KEY CONCEPT:
 *   Matching methods do not establish conformance, and pending proofs are facts
 *   only when seeded by an authentic parameter declaration.
 */
#include "xxir_constraint_proof.h"
#include "xxir_interface_members.h"
#include "xxir_types.h"
#include "xxir_implementation.h"
#include "xxir_generic.h"
#include "../base/xmalloc.h"

typedef struct XrXirConstraintEnvironment {
    const XrXirTypes *types;
    const XrXirConstraint *constraints;
    uint32_t parameter_count;
    const XrXirConstraint *own_constraints;
    uint32_t parent_count;
} XrXirConstraintEnvironment;
static const XrXirConstraint *constraint_fact(const XrXirConstraintEnvironment *environment, uint32_t parameter) {
    if (parameter >= environment->parameter_count) return NULL;
    return parameter < environment->parent_count ? &environment->constraints[parameter] :
        &environment->own_constraints[parameter-environment->parent_count];
}

static XrXirStatus constraint_equal(const XrXirConstraintEnvironment *environment,
    XrXirType type, uint64_t *work) {
    for (;;) {
        if (!work || !*work) return XR_XIR_BUDGET;
        --*work;
        uint32_t id = (uint32_t)type;
        if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING) return XR_XIR_OK;
        if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
            const XrXirConstraint *fact = constraint_fact(environment,id-XR_XIR_TYPE_PARAMETER_BASE);
            return fact && (fact->markers & XR_XIR_CONSTRAINT_EQUAL) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        }
        const XrXirTypeNode *node = xr_xir_type_node(environment->types,type);
        if (!node || (node->kind != XR_XIR_TYPE_ARRAY && node->kind != XR_XIR_TYPE_NULLABLE)) return XR_XIR_BAD_TYPE;
        if ((uint32_t)node->element >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
            (uint32_t)node->element < XR_XIR_CONSTRUCTED_TYPE_LIMIT && (uint32_t)node->element >= id)
            return XR_XIR_BAD_TYPE;
        type = node->element;
    }
}
static XrXirStatus constraint_markers(const XrXirConstraintEnvironment *environment,
    XrXirType type, uint32_t required, uint64_t *work) {
    const XrXirTypes *types = environment->types;
    if (required & ~XR_XIR_CONSTRAINT_MASK) return XR_XIR_BAD_TYPE;
    if (required & XR_XIR_CONSTRAINT_ERROR) {
        if (!work || !*work) return XR_XIR_BUDGET;
        --*work;
        uint32_t id = (uint32_t) type;
        bool parameter = id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT;
        const XrXirConstraint *fact = parameter ? constraint_fact(environment,id-XR_XIR_TYPE_PARAMETER_BASE) : NULL;
        if (parameter ? (!fact || !(fact->markers & XR_XIR_CONSTRAINT_ERROR)) :
            (type != XR_XIR_ERROR && !xr_xir_type_is_enum(types, type))) return XR_XIR_BAD_TYPE;
    }
    if (required & XR_XIR_CONSTRAINT_EQUAL) {
        XrXirStatus status = constraint_equal(environment,type,work);
        if (status != XR_XIR_OK) return status;
    }
    if (!(required & XR_XIR_CONSTRAINT_SENDABLE)) return XR_XIR_OK;
    for (;;) {
        if (!work || !*work) return XR_XIR_BUDGET;
        --*work;
        uint32_t id = (uint32_t) type;
        if (type == XR_XIR_UNIT || type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
            type == XR_XIR_ATOMIC_I64) return XR_XIR_OK;
        if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
            const XrXirConstraint *fact = constraint_fact(environment,id-XR_XIR_TYPE_PARAMETER_BASE);
            return fact && (fact->markers & XR_XIR_CONSTRAINT_SENDABLE) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        }
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node || (node->kind != XR_XIR_TYPE_ARRAY && node->kind != XR_XIR_TYPE_NULLABLE)) return XR_XIR_BAD_TYPE;
        if ((uint32_t) node->element >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
            (uint32_t) node->element < XR_XIR_CONSTRUCTED_TYPE_LIMIT && (uint32_t) node->element >= id)
            return XR_XIR_BAD_TYPE;
        type = node->element;
    }
}
static bool constraint_charge(XrXirBudget *budget, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX || bytes > budget->metadata_bytes || work > budget->work) return false;
    budget->metadata_bytes -= bytes; budget->work -= work; return true;
}
static bool constraint_argument_shape(const XrXirTypes *types, XrXirType type, uint32_t count) {
    if (type == XR_XIR_UNIT || xr_xir_type_is_cell(types, type)) return false;
    uint32_t id = (uint32_t)type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < count;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node ? node->parameter_span <= count : type == XR_XIR_BOOL || xr_xir_type_is_number(type) ||
        type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO;
}
XrXirStatus xr_xir_constraint_structure(const XrXirTypes *types, XrXirConstraint constraint,
    uint32_t count, XrXirBudget *budget) {
    if (!budget || (!!constraint.interfaces != !!constraint.interface_count)) return XR_XIR_BAD_STRUCTURE;
    if (constraint.markers & ~XR_XIR_CONSTRAINT_MASK) return XR_XIR_BAD_TYPE;
    if (!constraint_charge(budget, (uint64_t)constraint.interface_count * sizeof(*constraint.interfaces),
        (uint64_t)constraint.interface_count + 1)) return XR_XIR_BUDGET;
    const XrXirInterfaceTable *table = types ? types->interfaces : NULL;
    for (uint32_t i = 0; i < constraint.interface_count; ++i) {
        const XrXirInterfaceApplication *app = &constraint.interfaces[i];
        if (!table || !table->declarations || app->declaration >= table->count ||
            (!!app->arguments != !!app->argument_count)) return XR_XIR_BAD_STRUCTURE;
        if (app->argument_count != table->declarations[app->declaration].parameter_count) return XR_XIR_BAD_TYPE;
        if (!constraint_charge(budget, (uint64_t)app->argument_count * sizeof(*app->arguments), app->argument_count))
            return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < app->argument_count; ++a)
            if (!constraint_argument_shape(types, app->arguments[a], count)) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus constraint_environment(const XrXirModule *module, XrXirDeclarationContext owner,
    XrXirConstraintEnvironment *environment) {
    if (!module || (owner.kind != XR_XIR_CONTEXT_INTERFACE_METHOD && owner.member)) return XR_XIR_BAD_STRUCTURE;
    *environment = (XrXirConstraintEnvironment){module->types, NULL, 0, NULL, 0};
    if (owner.kind == XR_XIR_CONTEXT_FUNCTION) {
        if (!module->functions || owner.declaration >= module->function_count) return XR_XIR_BAD_STRUCTURE;
        if (module->generics) {
            const XrXirGeneric *g = &module->generics[owner.declaration];
            environment->constraints = g->constraints; environment->parameter_count = g->parameter_count;
        }
    } else if (owner.kind == XR_XIR_CONTEXT_NOMINAL) {
        const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
        if (!table || !table->declarations || owner.declaration >= table->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirNominalDeclaration *d = &table->declarations[owner.declaration];
        environment->constraints = d->constraints; environment->parameter_count = d->parameter_count;
    } else if (owner.kind == XR_XIR_CONTEXT_INTERFACE || owner.kind == XR_XIR_CONTEXT_INTERFACE_METHOD) {
        const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
        if (!table || !table->declarations || owner.declaration >= table->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirInterfaceDeclaration *d = &table->declarations[owner.declaration];
        environment->constraints = d->constraints; environment->parameter_count = d->parameter_count;
        if (owner.kind == XR_XIR_CONTEXT_INTERFACE_METHOD) {
            if (!d->methods || owner.member >= d->method_count) return XR_XIR_BAD_STRUCTURE;
            const XrXirInterfaceMethod *m = &d->methods[owner.member];
            if (d->parameter_count > 65536 || m->own_parameter_count > 65536-d->parameter_count ||
                (!!m->constraints != !!m->own_parameter_count)) return XR_XIR_BAD_STRUCTURE;
            environment->parent_count = d->parameter_count;
            environment->own_constraints = m->constraints;
            environment->parameter_count += m->own_parameter_count;
            return d->parameter_count && !d->constraints ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
        }
    } else if (owner.kind != XR_XIR_CONTEXT_CLOSED || owner.declaration) return XR_XIR_BAD_STRUCTURE;
    environment->parent_count = environment->parameter_count;
    return environment->parameter_count && !environment->constraints ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
typedef struct ConstraintObligation {
    struct ConstraintObligation *next;
    const XrXirModule *declaration_module;
    const XrXirTypes *requirement_types;
    XrXirConstraint requirement;
    const XrXirType *arguments;
    uint32_t argument_count;
    XrXirType subject;
    bool type_use;
} ConstraintObligation;
typedef struct ConstraintProof {
    const XrXirProofContext *context;
    XrXirConstraintEnvironment environment;
    ConstraintObligation *first, *last, *memory;
    XrXirBudget *budget;
    unsigned char *type_seen;
    uint64_t scratch_owned;
    XrXirInterfaceClosure *seed_closure;
    XrXirTypes seed_types;
    uint64_t seed_scratch;
} ConstraintProof;
static XrXirStatus proof_enqueue(ConstraintProof *proof, ConstraintObligation task) {
    if (!constraint_charge(proof->budget, 0, 1) || sizeof(task) > proof->budget->scratch_bytes)
        return XR_XIR_BUDGET;
    proof->budget->scratch_bytes -= sizeof(task); proof->scratch_owned += sizeof(task);
    ConstraintObligation *copy = xr_malloc(sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    *copy = task; copy->next = NULL;
    if (proof->last) proof->last->next = copy; else proof->first = proof->memory = copy;
    proof->last = copy; return XR_XIR_OK;
}
static void proof_dispose(ConstraintProof *proof) {
    xr_free(proof->type_seen);
    while (proof->memory) { ConstraintObligation *next = proof->memory->next;
        xr_free(proof->memory); proof->memory = next; }
    if (proof->budget) proof->budget->scratch_bytes += proof->scratch_owned;
    xr_xir_interface_closure_free(proof->seed_closure);
    if (proof->budget) proof->budget->scratch_bytes += proof->seed_scratch;
}
static XrXirStatus proof_type(ConstraintProof *proof, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(proof->environment.types,type);
    if (!node) return xr_xir_type_expression_shape(proof->environment.types,type,
        proof->environment.parameter_count,proof->budget);
    if (!constraint_charge(proof->budget,0,1)) return XR_XIR_BUDGET;
    if (node) {
        uint32_t count = proof->environment.types->count;
        if (!proof->type_seen) {
            if (count > proof->budget->scratch_bytes || !constraint_charge(proof->budget,0,count)) return XR_XIR_BUDGET;
            proof->budget->scratch_bytes -= count; proof->scratch_owned += count;
            proof->type_seen = xr_calloc(count,1);
            if (!proof->type_seen) return XR_XIR_OUT_OF_MEMORY;
        }
        uint32_t index = (uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (proof->type_seen[index]) return XR_XIR_OK;
        proof->type_seen[index] = 1;
    }
    ConstraintObligation task = {0}; task.type_use = true; task.subject = type;
    return proof_enqueue(proof,task);
}
static XrXirStatus proof_type_edge(ConstraintProof *proof, uint32_t earlier, XrXirType type) {
    if (xr_xir_type_node(proof->environment.types,type) &&
        (uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE >= earlier) return XR_XIR_BAD_TYPE;
    return proof_type(proof,type);
}
static XrXirStatus proof_arguments(ConstraintProof *proof, const XrXirModule *declaration_module,
    XrXirDeclarationContext owner, const XrXirType *arguments, uint32_t count) {
    XrXirConstraintEnvironment formal = {0};
    XrXirStatus status = constraint_environment(declaration_module,owner,&formal);
    if (status != XR_XIR_OK) return status;
    if (formal.parameter_count != count || (!!arguments != !!count)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < count; ++a) {
        if (!constraint_argument_shape(proof->environment.types,arguments[a],proof->environment.parameter_count))
            return XR_XIR_BAD_TYPE;
        ConstraintObligation task = {0}; task.declaration_module = declaration_module;
        task.requirement = *constraint_fact(&formal,a); task.arguments = arguments;
        task.argument_count = count; task.subject = arguments[a];
        status = proof_enqueue(proof,task);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus proof_nominal(ConstraintProof *proof, const XrXirModule *module,
    const XrXirTypeNode *node) {
    const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
    if (!table || node->nominal.declaration >= table->count) return XR_XIR_BAD_TYPE;
    /* Runtime identity-only pools have no declaration constraints. Their
     * source Checked obligations are checked by independent provenance. */
    if (!table->declarations) return table->identities ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    return proof_arguments(proof,module,(XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,node->nominal.declaration,0},
        node->nominal.arguments,node->nominal.argument_count);
}
static XrXirStatus proof_type_task(ConstraintProof *proof, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(proof->environment.types,type);
    if (!node) return type == XR_XIR_UNIT ||
        constraint_argument_shape(proof->environment.types,type,proof->environment.parameter_count) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    if (node->parameter_span > proof->environment.parameter_count) return XR_XIR_BAD_TYPE;
    uint32_t index = (uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirStatus status = XR_XIR_OK;
    /* Descriptor shapes have been admitted. Each edge and local nominal
     * obligation is visited once in this authentic context. TYPE deduplication
     * schedules shared subexpressions; conformance obligations are never facts
     * until the complete requirement queue succeeds. */
    if (node->kind == XR_XIR_TYPE_NOMINAL) {
        status = proof_nominal(proof,proof->context->module,node);
        for (uint32_t a = 0; status == XR_XIR_OK && a < node->nominal.argument_count; ++a)
            status = proof_type_edge(proof,index,node->nominal.arguments[a]);
    } else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_NULLABLE)
        status = proof_type_edge(proof,index,node->element);
    else if (node->kind == XR_XIR_TYPE_CALLABLE) {
        if (node->parameter_count && !node->parameters) return XR_XIR_BAD_STRUCTURE;
        status = proof_type_edge(proof,index,node->result);
        for (uint32_t a = 0; status == XR_XIR_OK && a < node->parameter_count; ++a)
            status = proof_type_edge(proof,index,node->parameters[a].type);
    } else return XR_XIR_BAD_TYPE;
    return status;
}
static XrXirStatus proof_match(ConstraintProof *proof, const ConstraintObligation *task,
    const XrXirInterfaceApplication *required, const XrXirInterfaceClosure *closure,
    const XrXirInterfaceApplication *fact) {
    if (!constraint_charge(proof->budget,0,1)) return XR_XIR_BUDGET;
    if (required->declaration != fact->declaration || required->argument_count != fact->argument_count)
        return XR_XIR_BAD_TYPE;
    for (uint32_t a = 0; a < fact->argument_count; ++a) {
        XrXirStatus status = xr_xir_type_substitution_matches_between(task->requirement_types ? task->requirement_types : task->declaration_module->types,
            xr_xir_interface_closure_types(closure), task->arguments, task->argument_count,
            required->arguments[a], fact->arguments[a], proof->budget);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus proof_concrete_closure(ConstraintProof *proof, const ConstraintObligation *task,
    const XrXirTypeNode *node, XrXirInterfaceClosure **output, uint64_t *scratch_owned) {
    const XrXirDeclarations *declarations = task->declaration_module->declarations;
    const XrXirImplementationTable *table = declarations ? declarations->implementations : NULL;
    if (!table) return XR_XIR_BAD_TYPE;
    if (!table->count || !table->records) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->count; ++i) {
        if (!constraint_charge(proof->budget,0,1)) return XR_XIR_BUDGET;
        count += table->records[i].nominal_declaration == node->nominal.declaration;
    }
    if (!count) return XR_XIR_BAD_TYPE;
    uint64_t bytes = (uint64_t)count * sizeof(XrXirInterfaceApplication);
    if (bytes > SIZE_MAX || bytes > proof->budget->scratch_bytes) return XR_XIR_BUDGET;
    proof->budget->scratch_bytes -= bytes;
    XrXirInterfaceApplication *roots = xr_malloc((size_t)bytes);
    if (!roots) { proof->budget->scratch_bytes += bytes; return XR_XIR_OUT_OF_MEMORY; }
    uint32_t at = 0;
    for (uint32_t i = 0; i < table->count; ++i)
        if (table->records[i].nominal_declaration == node->nominal.declaration)
            roots[at++] = table->records[i].interface;
    XrXirInterfaceClosureRequest request = {task->declaration_module->types,proof->environment.types,
        roots,count,node->nominal.arguments,node->nominal.argument_count,proof->environment.parameter_count};
    uint64_t before = proof->budget->scratch_bytes;
    XrXirStatus status = xr_xir_interface_closure_substitute(&request,proof->budget,output);
    *scratch_owned = before - proof->budget->scratch_bytes;
    xr_free(roots); proof->budget->scratch_bytes += bytes;
    if (status == XR_XIR_OK) status = proof_nominal(proof,task->declaration_module,node);
    return status;
}
static XrXirStatus proof_requirement_task(ConstraintProof *proof, const ConstraintObligation *task) {
    if (!constraint_argument_shape(proof->environment.types,task->subject,proof->environment.parameter_count))
        return XR_XIR_BAD_TYPE;
    XrXirStatus status = xr_xir_constraint_structure(task->requirement_types ? task->requirement_types : task->declaration_module->types,
        task->requirement,task->argument_count,proof->budget);
    if (status != XR_XIR_OK) return status;
    status = constraint_markers(&proof->environment,task->subject,task->requirement.markers,&proof->budget->work);
    if (status != XR_XIR_OK || !task->requirement.interface_count) return status;
    XrXirInterfaceClosure *closure = NULL; uint64_t closure_scratch = 0;
    uint32_t id = (uint32_t)task->subject;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        const XrXirConstraint *facts = constraint_fact(&proof->environment,id-XR_XIR_TYPE_PARAMETER_BASE);
        status = xr_xir_constraint_structure(proof->environment.types,*facts,
            proof->environment.parameter_count,proof->budget);
        if (status == XR_XIR_OK) {
            uint64_t before = proof->budget->scratch_bytes;
            XrXirInterfaceClosureRoots roots = {proof->environment.types ? proof->environment.types->interfaces : NULL,
                proof->environment.types,facts->interfaces,facts->interface_count,proof->environment.parameter_count};
            status = xr_xir_interface_closure_build(&roots,proof->budget,&closure);
            closure_scratch = before - proof->budget->scratch_bytes;
        }
    } else {
        const XrXirTypeNode *node = xr_xir_type_node(proof->environment.types,task->subject);
        status = node && node->kind == XR_XIR_TYPE_NOMINAL ?
            proof_concrete_closure(proof,task,node,&closure,&closure_scratch) : XR_XIR_BAD_TYPE;
    }
    for (uint32_t r = 0; status == XR_XIR_OK && r < task->requirement.interface_count; ++r) {
        bool matched = false;
        for (uint32_t a = 0; a < xr_xir_interface_closure_application_count(closure); ++a) {
            XrXirStatus match = proof_match(proof,task,&task->requirement.interfaces[r],closure,
                xr_xir_interface_closure_application(closure,a));
            if (match == XR_XIR_OK) { matched = true; break; }
            if (match != XR_XIR_BAD_TYPE) { status = match; break; }
        }
        if (status == XR_XIR_OK && !matched) status = XR_XIR_BAD_TYPE;
    }
    xr_xir_interface_closure_free(closure); proof->budget->scratch_bytes += closure_scratch; return status;
}
static XrXirStatus proof_run(ConstraintProof *proof, XrXirStatus status) {
    for (ConstraintObligation *task = proof->first; status == XR_XIR_OK && task; task = task->next) {
        if (!constraint_charge(proof->budget,0,1)) { status = XR_XIR_BUDGET; break; }
        status = task->type_use ? proof_type_task(proof,task->subject) : proof_requirement_task(proof,task);
    }
    proof_dispose(proof); return status;
}
static XrXirStatus proof_conformance_seed(ConstraintProof *proof) {
    const XrXirModule *module = proof->context->module;
    XrXirDeclarationContext owner = proof->context->owner;
    const XrXirImplementationTable *table = module && module->declarations ? module->declarations->implementations : NULL;
    const XrXirNominalTable *nominals = module && module->types ? module->types->nominals : NULL;
    const XrXirInterfaceTable *interfaces = module && module->types ? module->types->interfaces : NULL;
    if (!table || !table->records || owner.declaration >= table->count || !nominals || !nominals->declarations ||
        !interfaces || !interfaces->declarations) return XR_XIR_BAD_STRUCTURE;
    const XrXirImplementation *record = &table->records[owner.declaration];
    if (record->nominal_declaration >= nominals->count || !record->bindings || owner.member >= record->binding_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirImplementationBinding *binding = &record->bindings[owner.member];
    if (binding->requirement.declaration >= interfaces->count || binding->function >= module->function_count ||
        !module->functions || !module->declarations->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *interface = &interfaces->declarations[binding->requirement.declaration];
    if (!interface->methods || binding->member >= interface->method_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceMethod *method = &interface->methods[binding->member];
    const XrXirNominalDeclaration *nominal = &nominals->declarations[record->nominal_declaration];
    if (nominal->parameter_count > 65536 || method->own_parameter_count > 65536-nominal->parameter_count ||
        (nominal->parameter_count && !nominal->constraints)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = nominal->parameter_count+method->own_parameter_count;
    const XrXirFunctionIdentity *identity = &module->declarations->functions[binding->function];
    if (identity->nominal_owner != record->nominal_declaration+1 || identity->method_kind != XR_XIR_READ_METHOD ||
        identity->member_access != XR_XIR_MEMBER_PUBLIC || identity->cleanup_owner ||
        (module->generics ? module->generics[binding->function].parameter_count : 0) != count) return XR_XIR_BAD_TYPE;
    XrXirInterfaceClosureRoots roots = {interfaces,module->types,&binding->requirement,1,nominal->parameter_count};
    uint64_t before = proof->budget->scratch_bytes;
    XrXirStatus status = xr_xir_interface_closure_build(&roots,proof->budget,&proof->seed_closure);
    proof->seed_scratch = before-proof->budget->scratch_bytes;
    if (status != XR_XIR_OK) return status;
    const XrXirInterfaceRequirement *required = NULL;
    for (uint32_t r = 0; r < xr_xir_interface_closure_requirement_count(proof->seed_closure); ++r) {
        if (!constraint_charge(proof->budget,0,1)) return XR_XIR_BUDGET;
        const XrXirInterfaceRequirement *candidate = xr_xir_interface_closure_requirement(proof->seed_closure,r);
        if (!candidate->application && candidate->origin_interface == binding->requirement.declaration &&
            candidate->member == binding->member) required = candidate;
    }
    if (!required) return XR_XIR_BAD_STRUCTURE;
    proof->seed_types = *xr_xir_interface_closure_types(proof->seed_closure);
    proof->seed_types.interfaces = interfaces;
    proof->environment = (XrXirConstraintEnvironment){&proof->seed_types,nominal->constraints,count,
        required->constraints,nominal->parameter_count};
    return XR_XIR_OK;
}
static XrXirStatus proof_begin(ConstraintProof *proof, const XrXirProofContext *context, XrXirBudget *budget) {
    if (!context || !budget) return XR_XIR_BAD_STRUCTURE;
    *proof = (ConstraintProof){0}; proof->context = context; proof->budget = budget;
    if (context->owner.kind == XR_XIR_CONTEXT_CONFORMANCE_METHOD) return proof_conformance_seed(proof);
    return constraint_environment(context->module,context->owner,&proof->environment);
}
XrXirStatus xr_xir_constraints_prove(const XrXirProofContext *context,
    const XrXirConstraintUse *use, XrXirBudget *budget) {
    ConstraintProof proof = {0};
    XrXirStatus status = proof_begin(&proof,context,budget);
    if (status != XR_XIR_OK || !use || !use->declaration_module)
        return proof_run(&proof,status == XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : status);
    XrXirConstraintEnvironment formal = {0};
    status = constraint_environment(use->declaration_module,use->declaration,&formal);
    if (status != XR_XIR_OK) return proof_run(&proof,status);
    if (use->argument_count != formal.parameter_count || use->parameter >= use->argument_count || !use->arguments)
        return proof_run(&proof,XR_XIR_BAD_STRUCTURE);
    for (uint32_t a = 0; status == XR_XIR_OK && a < use->argument_count; ++a) {
        bool result = use->declaration.kind == XR_XIR_CONTEXT_FUNCTION && use->declaration_module->generics &&
            xr_xir_binder_kind(&use->declaration_module->generics[use->declaration.declaration],a) == XR_XIR_BINDER_RESULT_VARIABLE;
        const XrXirConstraint *fact = constraint_fact(&formal,a);
        if (result && (fact->markers || fact->interface_count || fact->interfaces)) status = XR_XIR_BAD_TYPE;
        else if (result && use->arguments[a] == XR_XIR_UNIT) continue;
        else if (!constraint_argument_shape(proof.environment.types,use->arguments[a],proof.environment.parameter_count))
            status = XR_XIR_BAD_TYPE;
        else status = proof_type(&proof,use->arguments[a]);
    }
    if (use->declaration.kind == XR_XIR_CONTEXT_FUNCTION && use->declaration_module->generics &&
        xr_xir_binder_kind(&use->declaration_module->generics[use->declaration.declaration],use->parameter) == XR_XIR_BINDER_RESULT_VARIABLE)
        return proof_run(&proof,status);
    ConstraintObligation task = {0}; task.declaration_module = use->declaration_module;
    task.requirement = *constraint_fact(&formal,use->parameter); task.arguments = use->arguments;
    task.argument_count = use->argument_count; task.subject = use->arguments[use->parameter];
    if (status == XR_XIR_OK) status = proof_enqueue(&proof,task);
    return proof_run(&proof,status);
}
XrXirStatus xr_xir_type_use_verify(const XrXirProofContext *context, XrXirType type, XrXirBudget *budget) {
    ConstraintProof proof = {0}; XrXirStatus status = proof_begin(&proof,context,budget);
    if (status == XR_XIR_OK) status = proof_type(&proof,type);
    return proof_run(&proof,status);
}
XR_FUNC XrXirStatus xr_xir_type_storage_prove(const XrXirProofContext *context,
    XrXirType type, XrXirBudget *budget) {
    ConstraintProof proof = {0};
    XrXirStatus status = proof_begin(&proof,context,budget);
    if (status == XR_XIR_OK && !constraint_argument_shape(proof.environment.types,type,
        proof.environment.parameter_count)) status = XR_XIR_BAD_TYPE;
    if (status == XR_XIR_OK) status = proof_type(&proof,type);
    return proof_run(&proof,status);
}
XrXirStatus xr_xir_type_markers_prove(const XrXirProofContext *context,
    XrXirType type, uint32_t markers, XrXirBudget *budget) {
    ConstraintProof proof = {0}; XrXirStatus status = proof_begin(&proof,context,budget);
    if (status != XR_XIR_OK) return proof_run(&proof,status);
    if (type != XR_XIR_UNIT && !constraint_argument_shape(proof.environment.types,type,proof.environment.parameter_count))
        return proof_run(&proof,XR_XIR_BAD_TYPE);
    status = proof_type(&proof,type);
    if (status == XR_XIR_OK) status = constraint_markers(&proof.environment,type,markers,&budget->work);
    return proof_run(&proof,status);
}
XrXirStatus xr_xir_interface_prove(const XrXirProofContext *context,
    const XrXirModule *declaration_module, XrXirType subject,
    XrXirInterfaceApplication application, XrXirBudget *budget) {
    ConstraintProof proof = {0}; XrXirStatus status = proof_begin(&proof,context,budget);
    if (status != XR_XIR_OK || !declaration_module)
        return proof_run(&proof,status == XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : status);
    uint32_t count = proof.environment.parameter_count;
    uint64_t bytes = (uint64_t)count * sizeof(XrXirType);
    if (bytes > SIZE_MAX || bytes > budget->scratch_bytes || count > budget->work) return proof_run(&proof,XR_XIR_BUDGET);
    budget->scratch_bytes -= bytes; budget->work -= count;
    XrXirType *arguments = count ? xr_malloc((size_t)bytes) : NULL;
    if (count && !arguments) { budget->scratch_bytes += bytes; return proof_run(&proof,XR_XIR_OUT_OF_MEMORY); }
    for (uint32_t a = 0; a < count; ++a) arguments[a] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + a);
    XrXirTypes actual = proof.environment.types ? *proof.environment.types : (XrXirTypes){0};
    actual.interfaces = declaration_module->types ? declaration_module->types->interfaces : NULL;
    ConstraintObligation task = {0}; task.declaration_module = declaration_module;
    task.requirement_types = &actual; task.requirement = (XrXirConstraint){0,&application,1};
    task.subject = subject; task.arguments = arguments; task.argument_count = count;
    status = proof_type(&proof,subject);
    for (uint32_t a = 0; status == XR_XIR_OK && a < application.argument_count; ++a) {
        if (!application.arguments) { status = XR_XIR_BAD_STRUCTURE; break; }
        status = proof_type(&proof,application.arguments[a]);
    }
    if (status == XR_XIR_OK) status = proof_enqueue(&proof,task);
    status = proof_run(&proof,status); xr_free(arguments); budget->scratch_bytes += bytes; return status;
}
static XrXirStatus proof_application_use(const XrXirProofContext *context,
    XrXirInterfaceApplication app, XrXirBudget *budget) {
    for (uint32_t a = 0; a < app.argument_count; ++a) {
        XrXirConstraintUse use = {context->module,{XR_XIR_CONTEXT_INTERFACE,app.declaration,0},a,
            app.arguments,app.argument_count};
        XrXirStatus status = xr_xir_constraints_prove(context,&use,budget);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_context_constraints_verify(const XrXirProofContext *context, XrXirBudget *budget) {
    XrXirConstraintEnvironment environment = {0};
    if (!context || !budget) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = constraint_environment(context->module,context->owner,&environment);
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < environment.parameter_count; ++p) {
        XrXirConstraint constraint = *constraint_fact(&environment,p);
        status = xr_xir_constraint_structure(environment.types,constraint,environment.parameter_count,budget);
        if (status != XR_XIR_OK) return status;
        if (!constraint.interface_count) continue;
        for (uint32_t a = 0; a < constraint.interface_count; ++a) {
            status = proof_application_use(context,constraint.interfaces[a],budget);
            if (status != XR_XIR_OK) return status;
        }
        XrXirInterfaceClosure *closure = NULL;
        uint64_t before = budget->scratch_bytes;
        XrXirInterfaceClosureRoots roots = {environment.types ? environment.types->interfaces : NULL,
            environment.types,constraint.interfaces,constraint.interface_count,environment.parameter_count};
        status = xr_xir_interface_closure_build(&roots,budget,&closure);
        uint64_t reserved = before - budget->scratch_bytes;
        xr_xir_interface_closure_free(closure); budget->scratch_bytes += reserved;
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus proof_nominal_declaration(const XrXirModule *module, uint32_t index, XrXirBudget *budget) {
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_NOMINAL,index,0}};
    const XrXirNominalDeclaration *d = &module->types->nominals->declarations[index];
    XrXirStatus status = xr_xir_context_constraints_verify(&context,budget);
    for (uint32_t f = 0; status == XR_XIR_OK && f < d->field_count; ++f)
        status = d->kind == XR_XIR_NOMINAL_CLASS ?
            xr_xir_type_storage_prove(&context,d->fields[f].type,budget) :
            xr_xir_type_use_verify(&context,d->fields[f].type,budget);
    return status;
}
static XrXirStatus proof_interface_declaration(const XrXirModule *module, uint32_t index, XrXirBudget *budget) {
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_INTERFACE,index,0}};
    const XrXirInterfaceDeclaration *d = &module->types->interfaces->declarations[index];
    XrXirStatus status = xr_xir_context_constraints_verify(&context,budget);
    for (uint32_t p = 0; status == XR_XIR_OK && p < d->parent_count; ++p)
        status = proof_application_use(&context,d->parents[p],budget);
    for (uint32_t m = 0; status == XR_XIR_OK && m < d->method_count; ++m) {
        context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_INTERFACE_METHOD,index,m};
        status = xr_xir_context_constraints_verify(&context,budget);
        if (status == XR_XIR_OK) status = xr_xir_type_use_verify(&context,d->methods[m].signature,budget);
    }
    return status;
}
/* Static methods carry nominal binders without an implicit receiver type.
 * Prove the inherited requirements in the actual function environment; neither
 * the nominal declaration nor a matching binder index grants those facts. */
static XrXirStatus proof_static_owner(const XrXirProofContext *context, XrXirBudget *budget) {
    const XrXirModule *module = context->module;
    if (!module->declarations || module->provenance) return XR_XIR_OK;
    const XrXirFunctionIdentity *identity = &module->declarations->functions[context->owner.declaration];
    if (identity->method_kind != XR_XIR_STATIC_METHOD) return XR_XIR_OK;
    if (!module->types->nominals->declarations) return XR_XIR_OK;
    const XrXirNominalDeclaration *owner = &module->types->nominals->declarations[identity->nominal_owner-1];
    uint32_t count = owner->parameter_count;
    if (!count) return XR_XIR_OK;
    if (count > budget->work) return XR_XIR_BUDGET;
    budget->work -= count;
    bool required = false;
    for (uint32_t a = 0; a < count; ++a)
        required |= owner->constraints[a].markers != 0 || owner->constraints[a].interface_count != 0;
    if (!required) return XR_XIR_OK;
    uint64_t bytes = (uint64_t)count*sizeof(XrXirType);
    uint64_t work = (uint64_t)count*2; /* Fill plus complete obligation scan. */
    if (bytes > SIZE_MAX || bytes > budget->scratch_bytes || work > budget->work) return XR_XIR_BUDGET;
    budget->scratch_bytes -= bytes; budget->work -= work;
    XrXirType *arguments = xr_malloc((size_t)bytes);
    if (!arguments) { budget->scratch_bytes += bytes; return XR_XIR_OUT_OF_MEMORY; }
    for (uint32_t a = 0; a < count; ++a) arguments[a] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a);
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t a = 0; status == XR_XIR_OK && a < count; ++a) {
        if (!owner->constraints[a].markers && !owner->constraints[a].interface_count) continue;
        XrXirConstraintUse use = {module,{XR_XIR_CONTEXT_NOMINAL,identity->nominal_owner-1,0},a,arguments,count};
        status = xr_xir_constraints_prove(context,&use,budget);
    }
    xr_free(arguments); budget->scratch_bytes += bytes; return status;
}
static XrXirStatus proof_function_declaration(const XrXirModule *module, uint32_t index, XrXirBudget *budget) {
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_FUNCTION,index,0}};
    const XrXirFunction *function = &module->functions[index];
    if (function->parameter_count && !function->parameters) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_method_signature_verify(module,index,budget);
    if (status == XR_XIR_OK) status = xr_xir_context_constraints_verify(&context,budget);
    if (status == XR_XIR_OK) status = proof_static_owner(&context,budget);
    for (uint32_t p = 0; status == XR_XIR_OK && p < function->parameter_count; ++p)
        status = xr_xir_type_use_verify(&context,function->parameters[p],budget);
    if (status == XR_XIR_OK) status = xr_xir_type_use_verify(&context,function->result,budget);
    const XrXirGeneric *g = module->generics ? &module->generics[index] : NULL;
    for (uint32_t a = 0; status == XR_XIR_OK && g && a < g->argument_count; ++a) {
        if (xr_xir_type_is_cell(module->types,g->arguments[a])) return XR_XIR_BAD_TYPE;
        status = g->arguments[a] == XR_XIR_UNIT ? xr_xir_result_unit_use(module,index,a,budget) :
            xr_xir_type_use_verify(&context,g->arguments[a],budget);
    }
    return status;
}
XrXirStatus xr_xir_declaration_constraints_verify(const XrXirModule *module, XrXirBudget *budget) {
    if (!module || !budget || (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    const XrXirNominalTable *nominals = module->types ? module->types->nominals : NULL;
    const XrXirInterfaceTable *interfaces = module->types ? module->types->interfaces : NULL;
    for (uint32_t d = 0; status == XR_XIR_OK && nominals && nominals->declarations && d < nominals->count; ++d)
        status = proof_nominal_declaration(module,d,budget);
    for (uint32_t d = 0; status == XR_XIR_OK && interfaces && d < interfaces->count; ++d)
        status = proof_interface_declaration(module,d,budget);
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f)
        status = proof_function_declaration(module,f,budget);
    XrXirProofContext closed = {module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    for (uint32_t n = 0; status == XR_XIR_OK && module->types && n < module->types->count; ++n)
        if (!module->types->nodes[n].parameter_span && module->types->nodes[n].kind == XR_XIR_TYPE_NOMINAL)
            status = xr_xir_type_use_verify(&closed,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n),budget);
    return status;
}
XrXirStatus xr_xir_module_constraints_verify(const XrXirModule *module, XrXirBudget *budget) {
    if (!module || !budget || (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    /* Complete signature structure was admitted before this module pass.
     * A scalar pool without generics has no declaration constraint obligations. */
    XrXirStatus status = !module->types && !module->generics ? XR_XIR_OK :
        xr_xir_declaration_constraints_verify(module,budget);
    if (status != XR_XIR_OK) return status;
    XrXirProofContext closed = {module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    const XrXirDeclarations *declarations = module->declarations;
    for (uint32_t s = 0; status == XR_XIR_OK && declarations && s < declarations->slot_count; ++s) {
        const XrXirSlot *slot = &declarations->slots[s];
        status = xr_xir_type_use_verify(&closed,slot->type,budget);
        if (status == XR_XIR_OK && slot->module != declarations->root_module)
            status = xr_xir_type_markers_prove(&closed,slot->type,XR_XIR_CONSTRAINT_SENDABLE,budget);
    }
    return status;
}
