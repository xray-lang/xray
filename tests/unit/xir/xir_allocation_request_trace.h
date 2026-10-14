/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_allocation_request_trace.h - Exact allocator request sequence witnesses
 *
 * KEY CONCEPT:
 *   Fixed test storage records requests before failure injection without adding
 *   implementation allocations or changing their ordinals.
 */
#ifndef XIR_ALLOCATION_REQUEST_TRACE_H
#define XIR_ALLOCATION_REQUEST_TRACE_H
typedef struct AllocationRequest { unsigned kind;size_t first,second;bool null_input; } AllocationRequest;
typedef struct AllocationRequests { AllocationRequest requests[20000];size_t count; } AllocationRequests;
static AllocationRequests allocation_whole_requests,allocation_prefix_requests;
static AllocationRequests allocation_reference_requests,allocation_runtime_requests;
static AllocationRequests *allocation_recording;
static void allocation_request_begin(AllocationRequests *requests) {
    CHECK(!allocation_recording && requests);requests->count=0;allocation_recording=requests;
}
static void allocation_request_end(void) { CHECK(allocation_recording);allocation_recording=NULL; }
static void allocation_request_record(unsigned kind,size_t first,size_t second,bool null_input) {
    if(!allocation_recording)return;
    CHECK(allocation_recording->count<20000);
    allocation_recording->requests[allocation_recording->count++]=(AllocationRequest){kind,first,second,null_input};
}
static void allocation_request_equal(const AllocationRequests *expected,size_t offset,
    const AllocationRequests *actual,size_t begin,size_t count) {
    CHECK(offset<=expected->count && count<=expected->count-offset);
    CHECK(begin<=actual->count && count<=actual->count-begin);
    for(size_t index=0;index<count;++index) {
        const AllocationRequest *a=&expected->requests[offset+index],*b=&actual->requests[begin+index];
        if(a->kind!=b->kind || a->first!=b->first || a->second!=b->second || a->null_input!=b->null_input)
            fprintf(stderr,"allocator request mismatch at %zu: %u/%zu/%zu/%u vs %u/%zu/%zu/%u\n",
                index,a->kind,a->first,a->second,a->null_input,b->kind,b->first,b->second,b->null_input);
        CHECK(a->kind==b->kind && a->first==b->first && a->second==b->second && a->null_input==b->null_input);
    }
}
#endif // XIR_ALLOCATION_REQUEST_TRACE_H
