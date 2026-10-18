/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_cell_provenance.c - Actual Cell origins at borrowed root use
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_roots_internal.h"
#include "xir/xxir_cell_provenance_internal.h"
#include "tuple_owner_observer.h"
#include "xir_view_root_fixture.h"

enum { C_SHARED,C_MUT,C_STRING,C_DIRECT,C_REFERENCE,C_UNKNOWN,C_CAPTURE,C_MODULE,
    C_COPY,C_LOCAL,C_ESCAPE,C_UNKNOWN_COPY,C_WIDE_REF,C_WIDE_UNKNOWN,C_WIDE_PHI,C_WIDE_UNKNOWN_PHI,C_UNBOUND_READ,C_CLEANUP,C_CASES };
typedef struct ViewCellFixture {
    RootFixture root;
    XrXirType parameters[130];XrXirCallableParameter references[130];
    uint32_t function,value,view,invalid;
    XrXirStatus expected,prefix_expected;
} ViewCellFixture;
static uint32_t view_cell_phi(RootFixture *f) {
    uint32_t condition=root_op(f,1,(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1});
    root_op(f,1,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={condition},.targets={1,2}});
    root_op(f,1,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    root_op(f,1,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t at=root_group(f,1,(uint32_t[]){1,0,2,129},4);f->functions[1].block_count=4;
    return root_op(f,1,(XrXirInstruction){.op=XR_XIR_PHI,.type=ROOT_CELL,.args={at,4}});
}
static void view_cell_case(ViewCellFixture *c,uint32_t kind) {
    memset(c,0,sizeof(*c));RootFixture *f=&c->root;
    if (kind==C_MODULE) {root_prepare(f,16);c->function=0;c->value=9;c->view=10;return;}
    root_fixture(f);
    for (uint32_t p=0;p<4;++p) {f->functions[p].parameter_count=0;f->functions[p].parameters=NULL;}
    f->functions[0].parameter_count=1;f->functions[0].parameters=f->parameters[0];
    f->parameters[0][0]=kind==C_STRING?XR_XIR_STRING:ROOT_ARRAY;
    if (kind==C_STRING) {f->nodes[1].element=XR_XIR_STRING;f->nodes[3].element=XR_XIR_U8;}
    root_bounds(f,0);uint32_t root=root_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=ROOT_CELL,.args={0}});
    bool wide=kind>=C_WIDE_REF && kind<=C_WIDE_UNKNOWN_PHI;
    bool parameter=(kind>=C_DIRECT && kind<=C_CAPTURE) || kind==C_ESCAPE || kind==C_UNKNOWN_COPY || wide || kind==C_UNBOUND_READ || kind==C_CLEANUP;
    c->function=parameter?1u:0u;c->value=parameter?0u:root;
    if (parameter) {f->functions[1].parameter_count=1;f->functions[1].parameters=f->parameters[1];}
    if (wide) {
        for (uint32_t p=0;p<130;++p) {c->parameters[p]=ROOT_CELL;c->references[p]=(XrXirCallableParameter){ROOT_CELL,XR_PARAM_REF};}
        f->functions[1].parameter_count=130;f->functions[1].parameters=c->parameters;c->value=129;
        f->nodes[12].parameter_count=130;f->nodes[12].parameters=c->references;
    }
    if (parameter) root_bounds(f,1);
    if (kind==C_DIRECT || kind==C_ESCAPE) {
        uint32_t at=root_group(f,0,&root,1);
        if (kind==C_ESCAPE) f->functions[1].result=ROOT_CELL;
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.type=kind==C_ESCAPE?ROOT_CELL:XR_XIR_UNIT,.args={at,1},.immediate=1});
    } else if (kind==C_REFERENCE || kind==C_CAPTURE || kind==C_WIDE_REF || kind==C_WIDE_PHI) {
        uint32_t at=kind==C_CAPTURE?root_group(f,0,&root,1):0;
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=kind==C_CAPTURE?ROOT_CLOSURE:ROOT_REF_CALLABLE,
            .args={at,kind==C_CAPTURE?1u:0u},.immediate=1});
    }
    if (kind==C_COPY || kind==C_UNKNOWN_COPY) {
        c->invalid=f->functions[c->function].instruction_count;
        c->value=root_op(f,c->function,(XrXirInstruction){.op=XR_XIR_COPY,.type=ROOT_CELL,.args={c->value}});
    } else if (kind==C_LOCAL) {
        uint32_t slot=root_op(f,0,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=ROOT_CELL,.args={root}});
        c->value=root_op(f,0,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=ROOT_CELL,.args={slot}});
    }
    if (kind==C_WIDE_PHI || kind==C_WIDE_UNKNOWN_PHI) c->value=view_cell_phi(f);
    if (kind==C_UNBOUND_READ) c->view=root_op(f,1,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=ROOT_ARRAY,.args={0}});
    else c->view=root_view(f,c->function,c->value,false);
    if (kind==C_MUT) f->ops[0][c->view-f->functions[0].parameter_count].type=ROOT_MUT;
    if (kind==C_UNKNOWN) {c->expected=XR_XIR_UNRESOLVED;c->invalid=c->view-f->functions[1].parameter_count;}
    if (kind==C_UNKNOWN_COPY || kind==C_WIDE_UNKNOWN || kind==C_WIDE_UNKNOWN_PHI) {
        c->expected=XR_XIR_UNRESOLVED;c->invalid=c->view-f->functions[1].parameter_count;
    }
    if (kind==C_CLEANUP) {
        uint32_t at=root_group(f,0,&root,1);f->identities[1].cleanup_owner=1;
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.args={at,1},.targets={1},.immediate=1});
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={2}});f->functions[0].block_count=3;
    }
    for (uint32_t p=0;p<4;++p) {
        if (kind==C_ESCAPE && p==1) {
            c->expected=c->prefix_expected=XR_XIR_BAD_TYPE;c->invalid=UINT32_MAX;
            root_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}});
        } else root_op(f,p,(XrXirInstruction){.op=XR_XIR_RETURN});
        if (!f->functions[p].operand_count) f->functions[p].operands=NULL;
    }
    if (kind==C_CLEANUP) {
        f->blocks[0][0]=(XrXirBlock){.count=4};f->blocks[0][1]=(XrXirBlock){.first=4,.count=1,.frontier=4};
        f->blocks[0][2]=(XrXirBlock){.first=5,.count=1};
    }
    if (kind==C_WIDE_PHI || kind==C_WIDE_UNKNOWN_PHI) {
        f->blocks[1][0]=(XrXirBlock){.count=4};f->blocks[1][1]=(XrXirBlock){.first=4,.count=1};
        f->blocks[1][2]=(XrXirBlock){.first=5,.count=1};f->blocks[1][3]=(XrXirBlock){.first=6,.count=f->functions[1].instruction_count-6u};
    }
}
static XrXirStatus view_cell_sequence(const XrXirCompileContext *context,uint32_t kind) {
    ViewCellFixture c;view_cell_case(&c,kind);RootFixture *f=&c.root;unsigned char before[sizeof(c)];memcpy(before,&c,sizeof(c));
    XrXirConstruction *construction=NULL;XrXirCellProvenance *proof=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_construction(context,&f->types,&construction);if (status!=XR_XIR_OK) goto done;
    status=xir_compile_view_prepare_ordinary(context,&f->module,construction,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (c.prefix_expected!=XR_XIR_OK) {
        CHECK(status==c.prefix_expected && diagnostic.function==c.function && diagnostic.instruction==c.invalid);goto done;
    }
    if (status!=XR_XIR_OK) fprintf(stderr,"view Cell prefix kind=%u status=%u at %u/%u/%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    status=xr_xir_compile_cell_provenance_verified(context,&f->module,&proof,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (status!=c.expected) fprintf(stderr,"view Cell kind=%u status=%u expected=%u at %u/%u/%u\n",kind,status,c.expected,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==c.expected);
    if (status!=XR_XIR_OK) {CHECK(!proof && diagnostic.function==c.function && diagnostic.instruction==c.invalid);goto done;}
    uint32_t origin=xr_xir_cell_provenance_origin(proof,c.function,c.value);
    if (kind==C_DIRECT || kind==C_REFERENCE || kind==C_WIDE_REF || kind==C_WIDE_PHI) {
        CHECK(origin&XR_XIR_CELL_ORIGIN_SCOPED);CHECK(!(origin&XR_XIR_CELL_ORIGIN_UNKNOWN));
        CHECK(xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_SCOPED_REF);
        if (kind==C_WIDE_REF || kind==C_WIDE_PHI) {
            XrXirCellOriginView view={0};status=xr_xir_compile_cell_origin_view(context,proof,1,c.value,&view);
            if (status!=XR_XIR_OK) goto done;
            CHECK(view.parameter_count==130 && view.word_count==3 && (view.dependencies[2]&2u));
            CHECK((view.dependencies[0]&1u)==(kind==C_WIDE_PHI));
            CHECK(xr_xir_cell_provenance_role(proof,1,129)==XR_XIR_CELL_PROOF_SCOPED_REF);
        }
    } else if (kind==C_UNBOUND_READ) {
        CHECK(origin==XR_XIR_CELL_ORIGIN_SCOPED && xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_UNKNOWN);
    } else if (kind==C_CLEANUP) {
        CHECK(origin&XR_XIR_CELL_ORIGIN_SCOPED);CHECK(!(origin&XR_XIR_CELL_ORIGIN_UNKNOWN));
        CHECK(xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_LEXICAL_CLEANUP);
    } else if (kind==C_CAPTURE) {
        CHECK(origin==XR_XIR_CELL_ORIGIN_OWNED);CHECK(xr_xir_cell_provenance_role(proof,1,0)==XR_XIR_CELL_PROOF_OWNED_CAPTURE);
    } else CHECK(origin==(kind==C_MODULE?XR_XIR_CELL_ORIGIN_MODULE:XR_XIR_CELL_ORIGIN_OWNED));
    status=xir_compile_view_roots_verified(context,&f->module,NULL,NULL,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    CHECK(status==(kind==C_CAPTURE?XR_XIR_BAD_VALUE:XR_XIR_OK));
    status=XR_XIR_OK;
done:
    CHECK(!memcmp(before,&c,sizeof(c)));xr_xir_compile_cell_provenance_free(proof);xr_xir_compile_construction_free(construction);return status;
}
static void view_cell_resources(uint32_t kind,bool normal) {
    ViewCellFixture fixture;view_cell_case(&fixture,kind);XrXirStatus expected=fixture.expected;
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;size_t start=attempts;
    CHECK(view_cell_sequence(&context,kind)==expected);size_t points=attempts-start;XrCompileResourceStats measured=stats(&context);
    CHECK(points && points<4096);CHECK(view_cell_sequence(&context,kind)==expected);owner_free(&context,baseline);
    if (!normal) for (size_t point=0;point<points;++point) {
        context=owner_new(caps());baseline=stats(&context).live_bytes;fail_at=attempts+point;injected=false;
        XrXirStatus status=view_cell_sequence(&context,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"view Cell kind=%u FI=%zu/%zu status=%u\n",kind,point,points,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&context,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limit=caps();uint64_t *bound=axis==0?&limit.allocated_bytes:axis==1?&limit.live_bytes:&limit.work;
        *bound=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        context=owner_new(limit);baseline=stats(&context).live_bytes;
        CHECK(view_cell_sequence(&context,kind)==(delta?expected:XR_XIR_BUDGET));owner_free(&context,baseline);
    }
    printf("View Cell provenance kind=%u: %zu allocation points owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<C_CASES;++kind) view_cell_resources(kind,normal);
    puts("Cell origin admission does not override independent capture, lifetime or execution checks");return 0;
}
