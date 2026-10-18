/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool call_sort_before(CallWork *w) {
    for(size_t i=1;i<w->count;++i){CallEdge current=w->edges[i];size_t at=i;
        while(at){int order=0;if(!call_edge_order(w,&w->edges[at-1],&current,&order))return false;if(order<=0)break;
            if(!call_work(w,sizeof(current)))return false;w->edges[at]=w->edges[at-1];--at;}
        w->edges[at]=current;}
    return true;
}
