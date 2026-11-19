/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_admission_cases.h - Exact empty constructor admission with real homes
 */
#include "../lir/lir_set_binding_fixture.h"
static void set_new_admission(void) {
    SetLirFixture fixtures[]={set_lir_fixture(false),set_binding_formal_fixture(0)};
    for (size_t fixture=0;fixture<2;++fixture) {
        SetLirFixture c=fixtures[fixture]; OK(xr_lir_module_finish(c.module,NULL));
        XrRbcProgram *program=NULL;
        CHECK(xr_rbc_compile(c.module,c.entry,&program,NULL)==XR_RBC_COMPILE_OK && program);
        if (fixture) {
            const XrRbcModule *module=xr_rbc_program_view(program);
            bool checked=false;
            for (size_t f=0;f<module->function_count;++f) {
                const XrRbcValueCode *code=xr_rbc_module_value_code(module,(uint32_t)f);
                const XrRbcFunction *function=&module->functions[f];
                if (!code || !code->homes || code->homes->source_function!=c.entry.id) continue;
                uint32_t scalar=0;
                for (uint32_t v=1;v<=code->value_count && !scalar;++v)
                    if (rbc_value_scalar(code,function,v,XR_RBC_BITS)) scalar=v;
                CHECK(scalar);
                for (size_t n=0;n<code->access_count;++n) {
                    XrRbcValueAccess access=code->accesses[n];
                    if (access.operation!=XR_RBC_VALUE_SET || access.index!=XR_LIR_SET_NEW) continue;
                    CHECK(rbc_value_set_access_valid(module,code,function,access));
                    // Keep authentic homes, event and scalar SSA ids. Only the
                    // raw operation's arity changes; no owner or scope is forged.
                    uint32_t operands[]={scalar,scalar}; XrRbcValueCode candidate=*code;
                    candidate.arguments=operands; candidate.argument_count=2;
                    access.first=0; access.count=0;
                    CHECK(rbc_value_set_access_valid(module,&candidate,function,access));
                    access.count=1; CHECK(!rbc_value_set_access_valid(module,&candidate,function,access));
                    access.count=2; CHECK(!rbc_value_set_access_valid(module,&candidate,function,access));
                    checked=true;
                }
            }
            CHECK(checked);
        }
        xr_lir_module_destroy(c.module); uint64_t bits=0;
        CHECK(execute(program,NULL,0,&bits)==XR_RBC_OK && bits==(fixture ? 6u : 1u));
        xr_rbc_program_destroy(program); CHECK(!live_allocations);
    }
}
