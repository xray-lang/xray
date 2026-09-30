/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_constraint_proof_cases.h - Declaration-owned whole-vector entailment
 */
#ifndef XIR_CONSTRAINT_PROOF_CASES_H
#define XIR_CONSTRAINT_PROOF_CASES_H
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_generic.h"
#include "xir_interface_member_cases.h"

typedef struct ConstraintProofFixture {
    MemberFixture base;
    XrXirConstraint constraints[3][2], nominal_constraints[2];
    XrXirGeneric generics[3];
    XrXirFunction functions[3];
    XrXirNominalDeclaration nominals[2];
    XrXirNominalTable nominal_table;
    XrXirType parameters[2];
    XrXirInterfaceApplication required, fact;
    XrXirModule module;
    XrXirProofContext context;
    XrXirConstraintUse use;
} ConstraintProofFixture;
static void constraint_proof_fixture(ConstraintProofFixture *f) {
    memset(f,0,sizeof(*f)); member_fixture(&f->base);
    f->parameters[0] = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->parameters[1] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    f->required = (XrXirInterfaceApplication){0,f->parameters,1};
    f->fact = f->required;
    f->constraints[0][1].interfaces = &f->required; f->constraints[0][1].interface_count = 1;
    f->constraints[1][1].interfaces = &f->fact; f->constraints[1][1].interface_count = 1;
    for (uint32_t i = 0; i < 3; ++i) {
        f->generics[i].constraints = f->constraints[i]; f->generics[i].parameter_count = 2;
    }
    f->nominals[0].module = (XrXirLiteral){"m",1}; f->nominals[0].name = (XrXirLiteral){"Owner",5};
    f->nominals[0].constraints = f->nominal_constraints; f->nominals[0].parameter_count = 2;
    f->nominals[1].module = (XrXirLiteral){"m",1}; f->nominals[1].name = (XrXirLiteral){"Value",5};
    f->nominal_table = (XrXirNominalTable){f->nominals,2,NULL};
    f->base.types.nominals = &f->nominal_table; f->base.types.interfaces = &f->base.table;
    f->base.nodes[6] = (XrXirTypeNode){0}; f->base.nodes[6].kind = XR_XIR_TYPE_NOMINAL;
    f->base.nodes[6].nominal.declaration = 1;
    f->base.nodes[7] = (XrXirTypeNode){0}; f->base.nodes[7].kind = XR_XIR_TYPE_ARRAY;
    f->base.nodes[7].element = f->parameters[1]; f->base.nodes[7].parameter_span = 2;
    f->module.stage = XR_XIR_BUILT; f->module.functions = f->functions; f->module.function_count = 3;
    f->module.generics = f->generics; f->module.types = &f->base.types;
    f->context = (XrXirProofContext){&f->module,{XR_XIR_CONTEXT_FUNCTION,1,0}};
    f->use = (XrXirConstraintUse){&f->module,{XR_XIR_CONTEXT_FUNCTION,0,0},1,f->parameters,2};
}
static XrXirStatus constraint_proof_status(ConstraintProofFixture *f) {
    XrXirBudget budget = xr_xir_default_budget();
    uint64_t scratch = budget.scratch_bytes;
    XrXirStatus status = xr_xir_constraints_prove(&f->context,&f->use,&budget);
    CHECK(budget.scratch_bytes==scratch && !live); return status;
}
static void constraint_proof_vectors(void) {
    ConstraintProofFixture f; constraint_proof_fixture(&f);
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    XrXirType swapped[] = {f.parameters[1],f.parameters[0]}; f.use.arguments = swapped;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    f.use.arguments = f.parameters; CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    /* Same subject, wrong dependency argument: declaration slots cannot be confused. */
    XrXirType wrong[] = {f.parameters[1],f.parameters[1]}; f.use.arguments = wrong;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    f.use.arguments = f.parameters;
    f.constraints[0][1].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    f.constraints[1][1].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    f.constraints[1][1].interfaces = NULL; f.constraints[1][1].interface_count = 0;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
}
static void constraint_proof_owners(void) {
    ConstraintProofFixture f; constraint_proof_fixture(&f);
    f.constraints[0][0] = f.constraints[0][1]; f.constraints[1][0] = f.constraints[1][1];
    f.use.parameter = 0;
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    f.context.owner.declaration = 2; CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    f.context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,0,0};
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    XrXirType same[] = {f.parameters[0],f.parameters[0]}; f.use.arguments = same;
    f.context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_INTERFACE,1,0};
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    f.context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,1,0};
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    f.nominal_constraints[0] = f.constraints[0][0];
    f.use.declaration = (XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,0,0};
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    f.use.declaration = (XrXirDeclarationContext){XR_XIR_CONTEXT_INTERFACE,1,0};
    f.base.declarations[1].constraints = f.nominal_constraints;
    f.use.argument_count = 1; CHECK(constraint_proof_status(&f)==XR_XIR_OK);
}
static void constraint_proof_inheritance(void) {
    ConstraintProofFixture f; constraint_proof_fixture(&f);
    f.fact = (XrXirInterfaceApplication){3,f.parameters+1,1};
    f.constraints[1][0] = f.constraints[1][1]; f.constraints[1][1] = (XrXirConstraint){0};
    XrXirType arguments[] = {member_case_type(7),f.parameters[0]}; f.use.arguments = arguments;
    /* An existing Array<P1> must equal the closure's independently reified Array<P1>. */
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    arguments[0] = member_case_type(0); CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    XrXirType template_array = member_case_type(0); f.required.arguments = &template_array;
    arguments[0] = f.parameters[1]; CHECK(constraint_proof_status(&f)==XR_XIR_OK);
    f.fact.arguments = &f.base.concrete; f.required.arguments = f.parameters;
    arguments[0] = member_case_type(4);
    CHECK(constraint_proof_status(&f)==XR_XIR_OK);
}
typedef struct ConstraintBoundFixture {
    XrXirConstraint empty, bound;
    XrXirType parameter, box;
    XrXirInterfaceApplication marker, parent;
    XrXirInterfaceDeclaration interfaces[3];
    XrXirInterfaceTable interface_table;
    XrXirNominalField field;
    XrXirNominalDeclaration nominals[2];
    XrXirNominalTable nominal_table;
    XrXirTypeNode node;
    XrXirTypes types;
} ConstraintBoundFixture;
static void constraint_bound_fixture(ConstraintBoundFixture *f) {
    memset(f,0,sizeof(*f)); f->parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->box = member_case_type(0); f->marker = (XrXirInterfaceApplication){0,NULL,0};
    f->bound.interfaces = &f->marker; f->bound.interface_count = 1;
    f->parent = (XrXirInterfaceApplication){1,&f->box,1};
    f->interfaces[0] = (XrXirInterfaceDeclaration){{"m",1},{"Marker",6},1,NULL,0,NULL,0,NULL,0};
    f->interfaces[1] = (XrXirInterfaceDeclaration){{"m",1},{"Parent",6},1,&f->empty,1,NULL,0,NULL,0};
    f->interfaces[2] = (XrXirInterfaceDeclaration){{"m",1},{"Child",5},1,&f->bound,1,&f->parent,1,NULL,0};
    f->interface_table = (XrXirInterfaceTable){f->interfaces,3};
    f->field = (XrXirNominalField){{"value",5},f->box,0};
    for (uint32_t i = 0; i < 2; ++i) {
        f->nominals[i].module = (XrXirLiteral){"m",1}; f->nominals[i].exported = 1;
        f->nominals[i].constraints = &f->bound; f->nominals[i].parameter_count = 1;
    }
    f->nominals[0].name = (XrXirLiteral){"Box",3}; f->nominals[1].name = (XrXirLiteral){"Holder",6};
    f->nominals[1].fields = &f->field; f->nominals[1].field_count = 1;
    f->nominal_table = (XrXirNominalTable){f->nominals,2,NULL};
    f->node.kind = XR_XIR_TYPE_NOMINAL; f->node.parameter_span = 1;
    f->node.nominal.arguments = &f->parameter; f->node.nominal.argument_count = 1;
    f->types = (XrXirTypes){&f->node,1,&f->nominal_table,&f->interface_table};
}
static void constraint_proof_hidden_nominal_bounds(void) {
    for (unsigned attack = 0; attack < 3; ++attack) {
        ConstraintBoundFixture f; constraint_bound_fixture(&f);
        if (attack==1) f.interfaces[2].constraints = &f.empty;
        if (attack==2) f.nominals[1].constraints = &f.empty;
        XrXirBudget budget = xr_xir_default_budget();
        CHECK(xr_xir_types_structure_verify(&f.types,&budget)==XR_XIR_OK);
        XrXirModule module = {0}; module.types = &f.types;
        CHECK(xr_xir_module_constraints_verify(&module,&budget)==(attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK(!live);
    }
    for (unsigned valid = 0; valid < 2; ++valid) {
        ConstraintBoundFixture f; constraint_bound_fixture(&f);
        XrXirConstraint constraints[2] = {{0},{0}};
        if (valid) constraints[0] = f.bound;
        constraints[1].interfaces = &f.parent; constraints[1].interface_count = 1;
        XrXirGeneric generic = {constraints,2,NULL,0};
        XrXirFunction function = {0};
        XrXirModule module = {XR_XIR_BUILT,&function,1,NULL,&generic,&f.types,NULL};
        XrXirBudget budget = xr_xir_default_budget();
        CHECK(xr_xir_generics_structure_verify(&module,&budget)==XR_XIR_OK);
        CHECK(xr_xir_module_constraints_verify(&module,&budget)==(valid ? XR_XIR_OK : XR_XIR_BAD_TYPE));
        CHECK(!live);
    }
}
static void constraint_proof_conflicts_and_concrete(void) {
    for (unsigned order = 0; order < 2; ++order) {
        ConstraintProofFixture f; constraint_proof_fixture(&f);
        XrXirInterfaceApplication facts[] = {f.fact,{5,NULL,0}};
        if (order) { XrXirInterfaceApplication tmp = facts[0]; facts[0] = facts[1]; facts[1] = tmp; }
        f.constraints[1][1].interfaces = facts; f.constraints[1][1].interface_count = 2;
        /* Selecting the satisfied first requirement does not hide another same-name conflict. */
        CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    }
    ConstraintProofFixture f; constraint_proof_fixture(&f);
    XrXirType arguments[] = {XR_XIR_I64,XR_XIR_I64}; f.use.arguments = arguments;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    arguments[1] = member_case_type(6); CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
}
static void constraint_proof_resources(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        ConstraintProofFixture f; constraint_proof_fixture(&f);
        attempts = 0; fail_at = site ? site-1 : SIZE_MAX;
        XrXirBudget budget = xr_xir_default_budget(), original = budget;
        CHECK(xr_xir_constraints_prove(&f.context,&f.use,&budget)==(site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        if (site) CHECK(budget.work<=original.work && budget.metadata_bytes<=original.metadata_bytes &&
            budget.scratch_bytes<=original.scratch_bytes); else sites = attempts;
        CHECK(budget.scratch_bytes==original.scratch_bytes && !live);
    }
    fail_at = SIZE_MAX;
    ConstraintProofFixture f; constraint_proof_fixture(&f);
    XrXirBudget original = xr_xir_default_budget(), spent = original;
    CHECK(xr_xir_constraints_prove(&f.context,&f.use,&spent)==XR_XIR_OK);
    XrXirBudget exact = original;
    exact.work -= spent.work; exact.metadata_bytes -= spent.metadata_bytes;
    CHECK(spent.scratch_bytes==original.scratch_bytes);
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        XrXirBudget budget = exact;
        if (boundary==1) --budget.work;
        if (boundary==2) budget.scratch_bytes = 0;
        XrXirBudget before = budget;
        CHECK(xr_xir_constraints_prove(&f.context,&f.use,&budget)==(boundary ? XR_XIR_BUDGET : XR_XIR_OK));
        if (boundary) CHECK(budget.work<=before.work && budget.metadata_bytes<=before.metadata_bytes &&
            budget.scratch_bytes<=before.scratch_bytes);
        CHECK(budget.scratch_bytes==before.scratch_bytes);
        CHECK(!live);
    }
    printf("Contextual interface proofs: %zu allocation failure sites\n",sites);
}
static void constraint_proof_malformed(void) {
    for (unsigned attack = 0; attack < 5; ++attack) {
        ConstraintProofFixture f; constraint_proof_fixture(&f);
        if (attack==0) f.constraints[0][1].interfaces = NULL;
        if (attack==1) f.constraints[0][1].interface_count = 0;
        if (attack==2) f.required.arguments = NULL;
        if (attack==3) f.required.declaration = UINT32_MAX;
        if (attack==4) f.required.argument_count = 0;
        CHECK(constraint_proof_status(&f)!=XR_XIR_OK);
    }
    ConstraintProofFixture f; constraint_proof_fixture(&f);
    XrXirType arguments[] = {XR_XIR_I64,XR_XIR_UNIT}; f.use.arguments = arguments;
    CHECK(constraint_proof_status(&f)!=XR_XIR_OK);
    constraint_proof_fixture(&f); f.use.declaration_module = NULL;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_STRUCTURE);
    constraint_proof_fixture(&f); f.context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
    constraint_proof_fixture(&f);
    XrXirConstraint empty = {0}; XrXirGeneric generic = {&empty,1,NULL,0};
    XrXirFunction function = {0};
    XrXirModule actual = {XR_XIR_BUILT,&function,1,NULL,&generic,NULL,NULL};
    f.context = (XrXirProofContext){&actual,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    XrXirType actual_arguments[] = {XR_XIR_I64,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    f.use.arguments = actual_arguments;
    CHECK(constraint_proof_status(&f)==XR_XIR_BAD_TYPE);
}
static void constraint_proof_shared_type_walk(void) {
    enum { DEPTH = 160 };
    XrXirTypeNode nodes[DEPTH] = {0}; XrXirCallableParameter parameters[DEPTH][2];
    for (uint32_t i = 0; i < DEPTH; ++i) {
        XrXirType child = i ? member_case_type(i-1) : (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
        parameters[i][0] = parameters[i][1] = (XrXirCallableParameter){child,0};
        nodes[i].kind = XR_XIR_TYPE_CALLABLE; nodes[i].parameter_span = 1;
        nodes[i].parameters = parameters[i]; nodes[i].parameter_count = 2; nodes[i].result = child;
    }
    XrXirTypes types = {nodes,DEPTH,NULL,NULL}; XrXirConstraint empty = {0};
    XrXirGeneric generic = {&empty,1,NULL,0}; XrXirFunction function = {0};
    XrXirModule module = {XR_XIR_BUILT,&function,1,NULL,&generic,&types,NULL};
    XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    XrXirBudget structure = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types,&structure)==XR_XIR_OK);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 1600;
    CHECK(xr_xir_type_use_verify(&context,member_case_type(DEPTH-1),&budget)==XR_XIR_OK);
    CHECK(!live);
    context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
    budget = xr_xir_default_budget(); budget.work = 1600;
    CHECK(xr_xir_type_use_verify(&context,member_case_type(DEPTH-1),&budget)==XR_XIR_BAD_TYPE);
    CHECK(!live);
}
static void constraint_proof_interface_identity_work(void) {
    enum { COUNT = 16 };
    XrXirTypeNode signature = {0}; signature.kind = XR_XIR_TYPE_CALLABLE; signature.result = XR_XIR_I64;
    XrXirInterfaceMethod method = {{"measure",7},member_case_type(0),0,0,NULL};
    XrXirInterfaceDeclaration declaration = {{"m",1},{"Measure",7},1,NULL,0,NULL,0,&method,1};
    XrXirInterfaceTable interfaces = {&declaration,1};
    XrXirTypes types = {&signature,1,NULL,&interfaces};
    XrXirInterfaceApplication application = {0,NULL,0};
    XrXirConstraint constraints[COUNT] = {0}; constraints[0] = (XrXirConstraint){0,&application,1};
    XrXirGeneric generic = {constraints,COUNT,NULL,0}; XrXirFunction function = {0};
    XrXirModule module = {XR_XIR_BUILT,&function,1,NULL,&generic,&types,NULL};
    XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    XrXirBudget structure = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types,&structure)==XR_XIR_OK);
    CHECK(xr_xir_generics_structure_verify(&module,&structure)==XR_XIR_OK);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = COUNT-1;
    uint64_t scratch = budget.scratch_bytes; size_t before = attempts;
    CHECK(xr_xir_interface_prove(&context,&module,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        application,&budget)==XR_XIR_BUDGET);
    CHECK(budget.work==COUNT-1 && budget.scratch_bytes==scratch && attempts==before && !live);
    budget = xr_xir_default_budget(); uint64_t work = budget.work;
    CHECK(xr_xir_interface_prove(&context,&module,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        application,&budget)==XR_XIR_OK);
    CHECK(budget.work<=work-COUNT && budget.scratch_bytes==scratch && !live);
    budget = xr_xir_default_budget(); attempts = 0; fail_at = 0;
    CHECK(xr_xir_interface_prove(&context,&module,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        application,&budget)==XR_XIR_OUT_OF_MEMORY);
    CHECK(budget.work==work-COUNT && budget.scratch_bytes==scratch && !live);
    fail_at = SIZE_MAX;
}
static void constraint_proof_scalar_signatures(void) {
    const XrXirType invalid[] = {(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,(XrXirType)UINT32_MAX};
    XrXirBlock block = {0,1,0,0};
    XrXirInstruction op = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    for (uint32_t i = 0; i < 1 + 2*sizeof(invalid)/sizeof(*invalid); ++i) {
        XrXirType parameter = XR_XIR_I64;
        XrXirFunction function = {"identity",8,&parameter,1,XR_XIR_I64,&block,1,&op,1,NULL,0};
        if (i) {
            XrXirType forged = invalid[(i-1)/2];
            if (i%2) parameter = forged; else function.result = forged;
        }
        XrXirModule module = {XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL};
        XrXirArtifact *checked = NULL;
        CHECK(xr_xir_check(&module,NULL,&checked,NULL)==(i ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK((checked!=NULL)==!i);
        xr_xir_artifact_free(checked); CHECK(!live);
    }
}
static void constraint_proof_scalar_generic_forwarding(void) {
    XrXirType parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirConstraint callee = {.markers=XR_XIR_CONSTRAINT_SENDABLE}, caller = {0};
    XrXirGeneric generics[] = {{&callee,1,NULL,0},{&caller,1,&parameter,1}};
    uint32_t operand = 0;
    XrXirInstruction identity = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction forwarding[] = {
        {XR_XIR_CALL,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,{0,1},{0},0,{0,1}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock leaf = {0,1,0,0}, body = {0,2,0,0};
    XrXirFunction functions[] = {
        {"identity",8,&parameter,1,parameter,&leaf,1,&identity,1,NULL,0},
        {"forward",7,&parameter,1,parameter,&body,1,forwarding,2,&operand,1}};
    XrXirModule module = {XR_XIR_BUILT,functions,2,NULL,generics,NULL,NULL};
    for (uint32_t valid = 0; valid < 2; ++valid) {
        caller.markers = valid ? XR_XIR_CONSTRAINT_SENDABLE : 0;
        XrXirArtifact *checked = NULL;
        CHECK(xr_xir_check(&module,NULL,&checked,NULL)==(valid ? XR_XIR_OK : XR_XIR_BAD_TYPE));
        CHECK((checked!=NULL)==!!valid);
        xr_xir_artifact_free(checked); CHECK(!live);
    }
}
static void constraint_proof_scalar_slot_sharing(void) {
    XrXirInstruction op = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction entry[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock block = {0,1,0,0}, main_block = {0,2,0,0};
    XrXirFunction functions[] = {
        {"provider_init",13,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"root_init",9,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&main_block,1,entry,2,NULL,0}};
    uint32_t dependency = 0;
    XrXirSourceModule modules[] = {{"provider",8,NULL,0,0},{"root",4,&dependency,1,1}};
    XrXirFunctionIdentity identities[] = {{0},{.module=1},{.module=1}};
    XrXirSlot slot = {1,XR_XIR_ERROR,0};
    XrXirDeclarations declarations = {modules,2,identities,&slot,1,NULL,0,1,2,NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL};
    /* Declaration obligations precede initializer execution. Error and PanicInfo
     * are valid owned slot types, but neither can be shared from another module. */
    for (uint32_t type = 0; type < 3; ++type) for (uint32_t owner = 0; owner < 2; ++owner) {
        slot.type = type == 0 ? XR_XIR_ERROR : type == 1 ? XR_XIR_PANIC_INFO : XR_XIR_STRING;
        slot.module = owner;
        XrXirBudget budget = xr_xir_default_budget();
        CHECK(xr_xir_declarations_verify(&declarations,NULL,3,&budget)==XR_XIR_OK);
        for (uint32_t f = 0; f < 3; ++f)
            CHECK(xr_xir_type_expression_shape(NULL,functions[f].result,0,&budget)==XR_XIR_OK);
        uint64_t scratch = budget.scratch_bytes;
        CHECK(xr_xir_module_constraints_verify(&module,&budget)==
            (owner == 1 || type == 2 ? XR_XIR_OK : XR_XIR_BAD_TYPE));
        CHECK(budget.scratch_bytes==scratch && !live);
    }
}
static void constraint_proof_cases(void) {
    constraint_proof_vectors(); constraint_proof_owners(); constraint_proof_inheritance();
    constraint_proof_conflicts_and_concrete(); constraint_proof_resources(); constraint_proof_malformed();
    constraint_proof_hidden_nominal_bounds(); constraint_proof_shared_type_walk();
    constraint_proof_interface_identity_work();
    constraint_proof_scalar_signatures(); constraint_proof_scalar_generic_forwarding();
    constraint_proof_scalar_slot_sharing();
}
#endif // XIR_CONSTRAINT_PROOF_CASES_H
