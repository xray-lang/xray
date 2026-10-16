/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_unit_leaf_native.c - Actual generated C Unit argument boundary
 */
#include "xir/xxir_scalar.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
XR_FUNC XrXirRunStatus unit_boundary_f0(XrXirRunContext *,const XrXirValue *,uint32_t,XrXirValue *);

int main(void) {
    XrXirValue argument={0},output={0};XrXirRunContext context={3,8,0,0,0,0};
    CHECK(unit_boundary_f0(&context,&argument,1,&output)==XR_XIR_RUN_OK);
    CHECK(output.type==XR_XIR_I64 && output.payload==42 && !context.live_bytes && context.allocations==context.frees);
    const XrXirValue malformed[]={{XR_XIR_UNIT,1,0},{XR_XIR_UNIT,0,1},{XR_XIR_BOOL,0,0}};
    for (uint32_t i=0;i<3;++i) {
        context=(XrXirRunContext){3,8,0,0,0,0};
        CHECK(unit_boundary_f0(&context,&malformed[i],1,&output)==XR_XIR_RUN_BAD_ARGUMENT);
        CHECK(!output.type && !output.reserved && !output.payload && !context.allocations);
    }
    context=(XrXirRunContext){3,8,0,0,0,0};
    CHECK(unit_boundary_f0(&context,NULL,0,&output)==XR_XIR_RUN_BAD_ARGUMENT && !context.allocations);
    context=(XrXirRunContext){2,8,0,0,0,0};
    CHECK(unit_boundary_f0(&context,&argument,1,&output)==XR_XIR_RUN_STEP_LIMIT);
    CHECK(!output.type && !context.live_bytes && context.allocations==context.frees);
    context=(XrXirRunContext){3,7,0,0,0,0};
    CHECK(unit_boundary_f0(&context,&argument,1,&output)==XR_XIR_RUN_FRAME_LIMIT && !context.allocations);
    puts("generated C preserves logical Unit arity without a Unit frame slot");return 0;
}
