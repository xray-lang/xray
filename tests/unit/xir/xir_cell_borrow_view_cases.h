/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_borrow_view_cases.h - Complete current physical query receivers
 */
#ifndef XIR_CELL_BORROW_VIEW_CASES_H
#define XIR_CELL_BORROW_VIEW_CASES_H
typedef struct CellBorrowFixture {
    CellProofFunction function;XrXirCellProvenance proof;
    uint8_t origins[6],tracked[6];uint32_t parameters[1];
    uint64_t cells[6],classes[6];XrXirCellClassRoot roots[2];
} CellBorrowFixture;
typedef struct CellBorrowProbe {
    XrXirCellClassOriginView expected,actual;
    unsigned calls;XrXirStatus result;
} CellBorrowProbe;
static XrXirCompileContext cell_borrow_owner(uint64_t work) {
    XrCompileResourceLimits limits={8388608,4194304,work};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrCompileResourceStats cell_borrow_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
/* These literal vectors exercise physical data only. They manufacture no
 * Checked artifact, Cell proof admission, Class identity or permission. */
static void cell_borrow_fixture(const XrXirCompileContext *context,CellBorrowFixture *f,uint32_t variant) {
    memset(f,0,sizeof(*f));bool roots=!!(variant&128),scoped=!!(variant&32);
    uint64_t bits=roots?(variant>>3)&3:0;
    for(uint32_t v=0;v<6;++v){f->origins[v]=(variant&1?XR_XIR_CELL_ORIGIN_MODULE:0)|
            (variant&2?XR_XIR_CELL_ORIGIN_UNKNOWN:0)|(variant&4?XR_XIR_CELL_ORIGIN_OWNED:0)|
            (bits?XR_XIR_CELL_ORIGIN_ROOT_UNRESOLVED:0);
        f->classes[v]=bits;f->cells[v]=variant&64?1:0;f->tracked[v]=4;}
    f->roots[0]=(XrXirCellClassRoot){3,0};f->roots[1]=(XrXirCellClassRoot){4,1};
    f->function=(CellProofFunction){.count=6,.parameters=2,.stored=6,.scopes=scoped?1:0,.words=scoped?1:0,
        .class_count=roots?2:0,.class_words=roots?1:0};
    f->proof=(XrXirCellProvenance){.resources=context->resources,.function_count=1,.value_count=6,.functions=&f->function,
        .origins=f->origins,.tracked=f->tracked,.dependencies=f->cells,.scope_parameters=f->parameters,
        .class_count=roots?2:0,.class_dependency_count=roots?6:0,
        .class_roots=roots?f->roots:NULL,.class_dependencies=roots?f->classes:NULL};
}
static XrXirCellClassOriginView cell_borrow_literal(const CellBorrowFixture *f,uint32_t variant,uint32_t value) {
    bool roots=!!(variant&128),scoped=!!(variant&32);uint32_t mask=(variant&1?XR_XIR_CELL_ACCESS_ROOT:0)|
        ((variant&2) || (roots && ((variant>>3)&3))?XR_XIR_CELL_ACCESS_UNKNOWN:0);
    return (XrXirCellClassOriginView){.cells={mask,scoped?&f->cells[value]:NULL,scoped?1u:0u,
            scoped?f->parameters:NULL,scoped?1u:0u},
        .residual_intrinsic_mask=(variant&1?XR_XIR_CELL_ACCESS_ROOT:0)|(variant&2?XR_XIR_CELL_ACCESS_UNKNOWN:0),
        .dependencies=roots?&f->classes[value]:NULL,.word_count=roots?1u:0u,.roots=roots?f->roots:NULL,.root_count=roots?2u:0u};
}
static XrXirStatus cell_borrow_receive(void *owner,const XrXirCellClassOriginView *view) {
    CellBorrowProbe *probe=owner;CHECK(view && class_view_same(view,&probe->expected));
    probe->actual=*view;++probe->calls;return probe->result;
}
static XrXirStatus cell_borrow_execute(const XrXirCompileContext *context,CellBorrowFixture *f,
    uint32_t value,CellBorrowProbe *probe) {
    CellBorrowFixture held=*f;XrCompileResourceStats before=cell_borrow_stats(context);
    XrXirCellClassOriginConsumer consumer={probe,cell_borrow_receive};
    XrXirStatus status=xr_xir_compile_cell_class_origin_consume(context,&f->proof,0,value,&consumer);
    XrCompileResourceStats after=cell_borrow_stats(context);
    CHECK(!memcmp(f,&held,sizeof(held)) && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    return status;
}
static void cell_borrow_faults(const XrXirCompileContext *context) {
    for(unsigned fault=0;fault<17;++fault){CellBorrowFixture f;cell_borrow_fixture(context,&f,255);
        CellBorrowProbe probe={.expected=cell_borrow_literal(&f,255,0)};
        if(!fault)f.function.class_words=2;else if(fault==1)f.function.class_begin=3;
        else if(fault==2)f.function.class_count=3;else if(fault==3)f.function.class_dependency_begin=7;
        else if(fault==4)f.proof.class_dependency_count=5;else if(fault==5)f.function.stored=5;
        else if(fault==6)f.proof.class_roots=NULL;else if(fault==7)f.proof.class_dependencies=NULL;
        else if(fault==8)f.roots[0].place=1;else if(fault==9)f.roots[0].place=6;
        else if(fault==10)f.roots[0].value=6;else if(fault==11)f.tracked[0]=0;
        else if(fault==12)f.roots[1].place=3;else if(fault==13)f.classes[0]=UINT64_C(1)<<63;
        else if(fault==14)f.origins[0]&=~XR_XIR_CELL_ORIGIN_ROOT_UNRESOLVED;
        else if(fault==15)f.classes[0]=0;else f.function.class_words=0;
        CHECK(cell_borrow_execute(context,&f,0,&probe)==XR_XIR_BAD_STRUCTURE && !probe.calls);
        XrXirCellClassOriginView output={0},empty={0};
        CHECK(xr_xir_compile_cell_class_origin_view(context,&f.proof,0,0,&output)==XR_XIR_BAD_STRUCTURE && class_view_same(&output,&empty));
    }
    CellBorrowFixture f;cell_borrow_fixture(context,&f,255);CellBorrowProbe probe={.expected=cell_borrow_literal(&f,255,0)};
    XrXirCellClassOriginConsumer consumer={&probe,cell_borrow_receive};
    CHECK(xr_xir_compile_cell_class_origin_consume(context,&f.proof,1,0,&consumer)==XR_XIR_BAD_STRUCTURE && !probe.calls);
    CHECK(cell_borrow_execute(context,&f,6,&probe)==XR_XIR_BAD_STRUCTURE && !probe.calls);
    CHECK(xr_xir_compile_cell_class_origin_consume(context,NULL,0,0,&consumer)==XR_XIR_BAD_STRUCTURE && !probe.calls);
    CHECK(xr_xir_compile_cell_class_origin_consume(context,&f.proof,0,0,NULL)==XR_XIR_BAD_STRUCTURE && !probe.calls);
    consumer.owner=NULL;CHECK(xr_xir_compile_cell_class_origin_consume(context,&f.proof,0,0,&consumer)==XR_XIR_BAD_STRUCTURE);
    consumer.owner=&probe;consumer.view=NULL;
    CHECK(xr_xir_compile_cell_class_origin_consume(context,&f.proof,0,0,&consumer)==XR_XIR_BAD_STRUCTURE && !probe.calls);
    XrXirCompileContext foreign=cell_borrow_owner(4000000);XrCompileResourceStats before=cell_borrow_stats(&foreign);
    CHECK(cell_borrow_execute(&foreign,&f,0,&probe)==XR_XIR_BAD_STRUCTURE && !probe.calls && cell_borrow_stats(&foreign).work==before.work);
    xr_compile_resources_release(foreign.resources);
    for(unsigned field=0;field<10;++field){XrXirCellClassOriginView output=class_view_literal(1u<<field),held=output;
        uint64_t work=cell_borrow_stats(context).work;
        CHECK(xr_xir_compile_cell_class_origin_view(context,&f.proof,0,0,&output)==XR_XIR_BAD_STRUCTURE &&
            class_view_same(&output,&held) && cell_borrow_stats(context).work==work);}
    const XrXirStatus refused[]={XR_XIR_IO,XR_XIR_BAD_STRUCTURE,XR_XIR_BUDGET,XR_XIR_OUT_OF_MEMORY,XR_XIR_UNRESOLVED};
    for(size_t i=0;i<sizeof(refused)/sizeof(*refused);++i){probe.calls=0;probe.result=refused[i];
        CHECK(cell_borrow_execute(context,&f,0,&probe)==refused[i] && probe.calls==1);
        CHECK(cell_borrow_execute(context,&f,0,&probe)==refused[i] && probe.calls==2);}
}
static void cell_borrow_work_bounds(uint32_t variant) {
    XrXirCompileContext context=cell_borrow_owner(4000000);CellBorrowFixture f;cell_borrow_fixture(&context,&f,variant);
    CellBorrowProbe probe={.expected=cell_borrow_literal(&f,variant,0)};
    CHECK(cell_borrow_execute(&context,&f,0,&probe)==XR_XIR_OK && probe.calls==1);
    uint64_t exact=cell_borrow_stats(&context).work;xr_compile_resources_release(context.resources);
    for(uint64_t cap=1;cap<=exact;++cap){context=cell_borrow_owner(cap);cell_borrow_fixture(&context,&f,variant);
        probe=(CellBorrowProbe){.expected=cell_borrow_literal(&f,variant,0)};
        CHECK(cell_borrow_execute(&context,&f,0,&probe)==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET) && probe.calls==(cap==exact?1u:0u));
        if(cap<exact)CHECK(cell_borrow_execute(&context,&f,0,&probe)==XR_XIR_BUDGET && !probe.calls);
        xr_compile_resources_release(context.resources);
    }
}
static void cell_borrow_view_controls(void) {
    XrXirCompileContext context=cell_borrow_owner(4000000);XrCompileResourceStats before=cell_borrow_stats(&context);
    for(uint32_t variant=0;variant<256;++variant){CellBorrowFixture f;cell_borrow_fixture(&context,&f,variant);
        for(uint32_t value=0;value<6;++value){CellBorrowProbe probe={.expected=cell_borrow_literal(&f,variant,value)};
            uint64_t start=cell_borrow_stats(&context).work;
            CHECK(cell_borrow_execute(&context,&f,value,&probe)==XR_XIR_OK && probe.calls==1 && class_view_same(&probe.actual,&probe.expected));
            uint64_t borrowed=cell_borrow_stats(&context).work-start;XrXirCellClassOriginView output={0};start=cell_borrow_stats(&context).work;
            CHECK(xr_xir_compile_cell_class_origin_view(&context,&f.proof,0,value,&output)==XR_XIR_OK && class_view_same(&output,&probe.expected));
            uint64_t extra=21-(1+sizeof(void *));
#define CELL_BORROW_NONZERO(member) if(probe.expected.member)extra+=sizeof(probe.expected.member);
            CELL_BORROW_NONZERO(cells.intrinsic_mask) CELL_BORROW_NONZERO(cells.dependencies) CELL_BORROW_NONZERO(cells.word_count)
            CELL_BORROW_NONZERO(cells.parameters) CELL_BORROW_NONZERO(cells.parameter_count) CELL_BORROW_NONZERO(residual_intrinsic_mask)
            CELL_BORROW_NONZERO(dependencies) CELL_BORROW_NONZERO(word_count) CELL_BORROW_NONZERO(roots) CELL_BORROW_NONZERO(root_count)
#undef CELL_BORROW_NONZERO
            CHECK(cell_borrow_stats(&context).work-start==borrowed+extra);
        }
    }
    cell_borrow_faults(&context);cell_borrow_work_bounds(0);cell_borrow_work_bounds(255);
    XrCompileResourceStats after=cell_borrow_stats(&context);
    CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes && after.work<4000000);
    fprintf(stderr,"Cell borrowed views:256 independent literal combinations across6 values, all ten fields and original copied-view fee oracle, complete physical bytes unchanged,17 shape/root/tracked/tail/presence refusals plus bounds/foreign ledger/consumer validity, five callback statuses and repeated requests, every work unit through exact in empty/full cases before any callback, original ten occupied-output guards and publication controls retained, no allocation work%llu cap4000000 PASS\n",(unsigned long long)after.work);
    xr_compile_resources_release(context.resources);
}
#endif // XIR_CELL_BORROW_VIEW_CASES_H
