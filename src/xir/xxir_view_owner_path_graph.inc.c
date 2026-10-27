/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_owner_path_graph.inc.c - Finite mandatory normal-edge waypoints
 */
#include "xxir_view_owner_path_internal.h"

typedef struct ViewOwnerWaypoint { uint32_t block,predecessor,rank; } ViewOwnerWaypoint;
typedef struct ViewOwnerPath {
    ViewMayOwner may;
    const XirViewOwnerPathRequest *request;
    ViewOwnerWaypoint *points;
    uint32_t count;
    bool conflicting;
} ViewOwnerPath;

static XrXirStatus owner_path_choices(ViewOwnerPath *p) {
    AffineCheck *a=&p->may.affine;const XrXirFunction *f=a->function;
    for (uint32_t side=0;side<2;++side) {
        const XrXirViewLoanChoice *choices=side?p->request->right:p->request->left;
        uint32_t count=side?p->request->right_count:p->request->left_count;
        for (uint32_t i=0;i<count;++i) {

            XrXirViewLoanChoice choice=choices[i];
            if (!choice.block || choice.block>=f->block_count || choice.predecessor>=f->block_count)
                return XR_XIR_BAD_STRUCTURE;
            const XrXirBlock *block=f->blocks+choice.predecessor;
            const XrXirInstruction *term=f->instructions+block->first+block->count-1u;bool edge=false;
            for (uint32_t e=0;e<affine_rules[term->op].edges;++e) {
                edge|=term->targets[e]==choice.block;
            }
            if (!edge) return XR_XIR_BAD_STRUCTURE;
            bool found=false;
            for (uint32_t j=0;j<p->count;++j) {

                if (p->points[j].block==choice.block) {
                    found=true;p->conflicting|=p->points[j].predecessor!=choice.predecessor;
                }
            }
            if (!found) p->points[p->count++]=(ViewOwnerWaypoint){choice.block,choice.predecessor,0};
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus owner_path_edge(ViewOwnerPath *p,uint32_t b,uint32_t edge,bool filtered,
    uint32_t *target,bool *present) {
    AffineCheck *a=&p->may.affine;const XrXirBlock *block=a->function->blocks+b;
    const XrXirInstruction *term=a->function->instructions+block->first+block->count-1u;*present=false;

    if (edge<2?edge>=affine_rules[term->op].edges:!block->panic) return XR_XIR_OK;
    *target=edge<2?term->targets[edge]:block->panic;*present=true;
    if (filtered) for (uint32_t i=0;i<p->count;++i) {

        if (p->points[i].block==*target && (edge==2 || p->points[i].predecessor!=b)) *present=false;
    }
    return XR_XIR_OK;
}
static XrXirStatus owner_path_walk(ViewOwnerPath *p,uint32_t start,uint32_t stop,bool filtered,bool *repeated) {
    AffineCheck *a=&p->may.affine;uint32_t blocks=a->function->block_count;*repeated=false;

    memset(a->reachable,0,blocks);a->reachable[start]=1;a->queue[0]=start;uint32_t head=0,tail=1;
    while (head<tail) {
        uint32_t b=a->queue[head++];if (b==stop) continue;
        for (uint32_t e=0;e<3;++e) {
            uint32_t target=0;bool present=false;XrXirStatus status=owner_path_edge(p,b,e,filtered,&target,&present);
            if (status!=XR_XIR_OK) return status;if (!present) continue;
            *repeated|=target==start;
            if (!a->reachable[target]) {
                XR_CHECK(tail<blocks,"each conditional CFG block enters its finite queue once");
                a->reachable[target]=1;a->queue[tail++]=target;
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus owner_path_order(ViewOwnerPath *p) {
    AffineCheck *a=&p->may.affine;
    for (uint32_t i=0;i<p->count;++i) {
        bool repeated=false;XrXirStatus status=owner_path_walk(p,p->points[i].block,UINT32_MAX,false,&repeated);
        if (status!=XR_XIR_OK || repeated) return status!=XR_XIR_OK?status:XR_XIR_UNRESOLVED;
        for (uint32_t j=0;j<p->count;++j) {

            if (i!=j) p->points[i].rank+=a->reachable[p->points[j].block]!=0;
        }
    }
    for (uint32_t i=1;i<p->count;++i) {
        ViewOwnerWaypoint point=p->points[i];uint32_t j=i;
        while (j && p->points[j-1u].rank<point.rank) {
            p->points[j]=p->points[j-1u];--j;
        }
        p->points[j]=point;
    }
    return XR_XIR_OK;
}
