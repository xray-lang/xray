/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_owned_fixture.h - Nonempty declaration-owned method constraints
 *
 * KEY CONCEPT:
 *   Parent and method parameters occupy distinct ordinals in an authentic scope.
 */
#ifndef XIR_GENERIC_METHOD_OWNED_FIXTURE_H
#define XIR_GENERIC_METHOD_OWNED_FIXTURE_H
#include "xir/xxir_interface.h"
#include "xir/xxir_types.h"
#include <stddef.h>
typedef struct GenericMethodOwnedFixture {
    XrXirType parent;
    XrXirConstraint outer, own[2];
    XrXirInterfaceApplication applications[2];
    XrXirCallableParameter parameter;
    XrXirTypeNode signature;
    XrXirInterfaceMethod methods[2];
    XrXirInterfaceDeclaration declarations[2];
    XrXirInterfaceTable table;
    XrXirTypes types;
    XrXirInstruction ops[2], init;
    XrXirBlock block, init_block;
    XrXirFunction functions[2];
    XrXirSourceModule owner;
    XrXirFunctionIdentity identities[2];
    XrXirDeclarations program;
    XrXirModule module;
} GenericMethodOwnedFixture;
static void generic_method_owned_fixture(GenericMethodOwnedFixture *f) {
    memset(f,0,sizeof(*f));
    f->parent = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->applications[0] = (XrXirInterfaceApplication){0,&f->parent,1};
    f->applications[1] = f->applications[0];
    f->own[0] = (XrXirConstraint){XR_XIR_CONSTRAINT_SENDABLE,&f->applications[0],1};
    f->own[1] = (XrXirConstraint){0,&f->applications[1],1};
    f->parameter = (XrXirCallableParameter){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),0};
    f->signature.kind = XR_XIR_TYPE_CALLABLE; f->signature.parameter_span = 2;
    f->signature.parameters = &f->parameter; f->signature.parameter_count = 1;
    f->signature.result = f->parameter.type;
    f->methods[0] = (XrXirInterfaceMethod){{"map",3},(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0,1,&f->own[0]};
    f->methods[1] = (XrXirInterfaceMethod){{"copy",4},(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0,1,&f->own[1]};
    f->declarations[0] = (XrXirInterfaceDeclaration){{"alpha",5},{"Evidence",8},1,&f->outer,1,NULL,0,NULL,0};
    f->declarations[1] = (XrXirInterfaceDeclaration){{"alpha",5},{"Parent",6},1,&f->outer,1,NULL,0,f->methods,2};
    f->table = (XrXirInterfaceTable){f->declarations,2};
    f->types = (XrXirTypes){&f->signature,1,NULL,&f->table};
    f->ops[0] = (XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}};
    f->ops[1] = (XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->init = f->ops[1]; f->block = (XrXirBlock){0,2,0,0}; f->init_block = (XrXirBlock){0,1,0,0};
    f->functions[0] = (XrXirFunction){"main",4,NULL,0,XR_XIR_I64,&f->block,1,f->ops,2,NULL,0};
    f->functions[1] = (XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&f->init_block,1,&f->init,1,NULL,0};
    f->owner = (XrXirSourceModule){"alpha",5,NULL,0,1}; f->identities[0].exported = 1;
    f->program = (XrXirDeclarations){&f->owner,1,f->identities,NULL,0,NULL,0,0,0,NULL};
    f->module = (XrXirModule){XR_XIR_BUILT,f->functions,2,&f->program,NULL,&f->types,NULL};
}
static void generic_method_owned_assert(const XrXirInterfaceTable *table) {
    CHECK(table && table->count == 2);
    const XrXirInterfaceDeclaration *parent = &table->declarations[1];
    CHECK(parent->parameter_count == 1 && parent->constraints && !parent->constraints[0].markers);
    CHECK(parent->method_count == 2);
    for (uint32_t m = 0; m < 2; ++m) {
        const XrXirInterfaceMethod *method = &parent->methods[m];
        CHECK(method->own_parameter_count == 1 && method->constraints);
        CHECK(method->constraints[0].markers == (m ? 0u : XR_XIR_CONSTRAINT_SENDABLE));
        CHECK(method->constraints[0].interface_count == 1);
        const XrXirInterfaceApplication *application = method->constraints[0].interfaces;
        CHECK(application && application->declaration == 0 && application->argument_count == 1);
        CHECK(application->arguments[0] == (XrXirType)XR_XIR_TYPE_PARAMETER_BASE);
    }
}
_Static_assert(sizeof(void *) != 8 || sizeof(XrXirInterfaceMethod) == 40,"method metadata stride");
_Static_assert(sizeof(void *) != 8 || offsetof(XrXirInterfaceMethod,own_parameter_count) == 24,"method arity offset");
_Static_assert(sizeof(void *) != 8 || offsetof(XrXirInterfaceMethod,constraints) == 32,"method constraints offset");
#endif // XIR_GENERIC_METHOD_OWNED_FIXTURE_H
