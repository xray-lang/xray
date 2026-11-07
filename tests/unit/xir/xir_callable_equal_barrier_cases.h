/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_equal_barrier_cases.h - Complete same-bound loss admission
 *
 * KEY CONCEPT:
 *   Equal bounds still scan real parameters without retaining compiler memory.
 */
#ifndef XIR_CALLABLE_EQUAL_BARRIER_CASES_H
#define XIR_CALLABLE_EQUAL_BARRIER_CASES_H
static void bound_equal_raw_guards(void) {
    XrXirCompileContext context=bound_owner(bound_caps());
    XrXirCallableParameter parameters[]={{XR_XIR_I64,0},{XR_XIR_BOOL,0},{XR_XIR_STRING,0}};
    for (unsigned attack=0;attack<12;++attack) {
        XrXirTypeNode nodes[]={
            {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=parameters,.parameter_count=3,
             .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
            {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=parameters,.parameter_count=3,
             .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED}};
        XrXirTypes types={nodes,2,NULL,NULL};
        XrXirType target=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
        if (attack<3) {
            if (attack!=1) nodes[0].parameters=NULL;
            if (attack!=0) nodes[1].parameters=NULL;
            if (attack==2) target=XR_XIR_CONSTRUCTED_TYPE_BASE;
        } else if (attack<6) {
            nodes[0].parameter_count=nodes[1].parameter_count=0;
            if (attack==3) nodes[1].parameters=NULL;
            if (attack==4) nodes[0].parameters=NULL;
            if (attack==5) target=XR_XIR_CONSTRUCTED_TYPE_BASE;
        } else if (attack<10) {
            static const uint32_t invalid[]={0,1,10,24};
            nodes[0].flags=nodes[1].flags=invalid[attack-6];
            target=XR_XIR_CONSTRUCTED_TYPE_BASE;
        } else if (attack==10) nodes[1].parameter_count=2;
        else nodes[1].result=XR_XIR_BOOL;
        XrCompileResourceStats before=bound_stats(&context);
        CHECK(xr_xir_compile_callable_weakening(&context,&types,
            XR_XIR_CONSTRUCTED_TYPE_BASE,target)==XR_XIR_BAD_TYPE);
        XrCompileResourceStats after=bound_stats(&context);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
        CHECK(after.held_bytes==before.held_bytes);
    }
    bound_owner_free(&context);
}
static void bound_equal_complete_parameters(void) {
    XrXirCompileContext context=bound_owner(bound_caps());
    XrXirCallableParameter input[]={{XR_XIR_I64,0},{XR_XIR_BOOL,0},{XR_XIR_STRING,0}};
    XrXirCallableParameter output[3];memcpy(output,input,sizeof(input));
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=input,.parameter_count=3,
         .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=output,.parameter_count=3,
         .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED}};
    XrXirTypes types={nodes,2,NULL,NULL};
    for (unsigned attack=0;attack<3;++attack) {
        output[2]=input[2];
        if (attack==1) output[2].type=XR_XIR_BOOL;
        if (attack==2) output[2].mode=1;
        XrCompileResourceStats before=bound_stats(&context);
        CHECK(xr_xir_compile_callable_weakening(&context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE,
            (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1))==(attack?XR_XIR_BAD_TYPE:XR_XIR_OK));
        XrCompileResourceStats after=bound_stats(&context);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
        CHECK(after.held_bytes==before.held_bytes);
    }
    bound_owner_free(&context);
}
static void bound_equal_memory_reuse(void) {
    XrXirCallableParameter parameters[]={{XR_XIR_I64,0},{XR_XIR_BOOL,0},{XR_XIR_STRING,0}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=parameters,.parameter_count=3,
        .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types={&node,1,NULL,NULL};BoundMark mark=bound_mark();
    XrXirCompileContext context=bound_owner(bound_caps());uint64_t held=bound_creation.held_bytes;
    bound_owner_free(&context);bound_balanced(mark);
    XrCompileResourceLimits caps={XR_COMPILE_MEMORY_FINITE,held,true};context=bound_owner(caps);
    XrCompileResourceStats before=bound_stats(&context);
    for(unsigned repeat=0;repeat<2;++repeat) {
        CHECK(xr_xir_compile_callable_weakening(&context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE,
            XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_OK);
        XrCompileResourceStats after=bound_stats(&context);CHECK(!memcmp(&before,&after,sizeof(before)));
    }
    bound_owner_free(&context);bound_balanced(mark);
}
static void bound_equal_barrier_cases(void) {
    BoundMark mark=bound_mark();
    bound_equal_raw_guards();bound_balanced(mark);
    bound_equal_complete_parameters();bound_balanced(mark);
    bound_equal_memory_reuse();bound_balanced(mark);
    puts("same callable bounds: 12 malformed guards, complete parameters, finite held memory and repeatable validation");
}
#endif /* XIR_CALLABLE_EQUAL_BARRIER_CASES_H */
