/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_allocation_bookkeeping_cases.h - Independent physical allocation oracle
 *
 * KEY CONCEPT:
 *   Real allocations grow, move and retire while a separate pointer set checks
 *   every recorded byte count and lifetime identity, including failed realloc.
 */
#ifndef XIR_ALLOCATION_BOOKKEEPING_CASES_H
#define XIR_ALLOCATION_BOOKKEEPING_CASES_H

static void allocation_bookkeeping_verify(void *const *pointers,const size_t *sizes,
    const uint64_t *identities,size_t count) {
    size_t expected_live=0,expected_bytes=0;
    for(size_t i=0;i<count;++i) if(pointers[i]) {
        ++expected_live;expected_bytes+=sizes[i];
        size_t found=0;
        for(size_t j=0;j<live;++j) if(allocation_records[j].pointer==pointers[i]) {
            CHECK(allocation_records[j].bytes==sizes[i] && allocation_records[j].identity==identities[i]);
            ++found;
        }
        CHECK(found==1);
        const unsigned char *bytes=pointers[i];
        for(size_t j=0;j<sizes[i];++j)CHECK(bytes[j]==(unsigned char)(i+1));
    }
    CHECK(live==expected_live && class_live_bytes==expected_bytes);
    if(!live)CHECK(!allocation_records && !allocation_capacity);
}
static void allocation_bookkeeping_cases(void) {
    enum { count=768 };
    void *pointers[count]={0};size_t sizes[count]={0};uint64_t identities[count]={0};
    CHECK(!live && !class_live_bytes && !segment_probe_active && !allocation_recording);
    allocation_case_name="bookkeeping";allocation_stage="growth-move-retirement";
    for(unsigned round=0;round<3;++round) {
        for(size_t i=0;i<count;++i) {
            sizes[i]=i%61+1;uint64_t previous=allocation_identity;
            pointers[i]=i%2?counted_malloc(sizes[i]):counted_calloc(1,sizes[i]);
            CHECK(pointers[i] && allocation_identity==previous+1);identities[i]=allocation_identity;
            memset(pointers[i],(unsigned char)(i+1),sizes[i]);
        }
        allocation_bookkeeping_verify(pointers,sizes,identities,count);
        for(size_t i=round;i<count;i+=3) {counted_free(pointers[i]);pointers[i]=NULL;}
        allocation_bookkeeping_verify(pointers,sizes,identities,count);
        for(size_t i=0;i<count;i+=5) if(pointers[i]) {
            size_t length=4096+i;uint64_t previous=allocation_identity;
            uintptr_t old_address=(uintptr_t)pointers[i];
            void *replacement=counted_realloc(pointers[i],length);
            bool moved=(uintptr_t)replacement!=old_address;
            CHECK(replacement && allocation_identity==previous+moved);
            const unsigned char *bytes=replacement;
            for(size_t j=0;j<sizes[i];++j)CHECK(bytes[j]==(unsigned char)(i+1));
            if(moved)identities[i]=allocation_identity;
            pointers[i]=replacement;sizes[i]=length;memset(replacement,(unsigned char)(i+1),length);
        }
        allocation_bookkeeping_verify(pointers,sizes,identities,count);
        size_t occupied=0;while(occupied<count && !pointers[occupied])++occupied;CHECK(occupied<count);
        size_t before_live=live,before_bytes=class_live_bytes;uint64_t before_identity=allocation_identity;
        fail_at=allocation_calls;CHECK(!counted_malloc(19));fail_at=SIZE_MAX;
        fail_at=allocation_calls;CHECK(!counted_calloc(3,7));fail_at=SIZE_MAX;
        fail_at=allocation_calls;CHECK(!counted_realloc(pointers[occupied],8192));fail_at=SIZE_MAX;
        CHECK(live==before_live && class_live_bytes==before_bytes && allocation_identity==before_identity);
        counted_free(NULL);CHECK(live==before_live && class_live_bytes==before_bytes);
        allocation_bookkeeping_verify(pointers,sizes,identities,count);
        for(size_t i=0;i<count;++i) if(!pointers[i]) {
            sizes[i]=i%37+1;uint64_t previous=allocation_identity;
            pointers[i]=counted_realloc(NULL,sizes[i]);
            CHECK(pointers[i] && allocation_identity==previous+1);identities[i]=allocation_identity;
            memset(pointers[i],(unsigned char)(i+1),sizes[i]);
        }
        allocation_bookkeeping_verify(pointers,sizes,identities,count);
        for(size_t i=0;i<count;++i) {
            size_t index=(i*509)%count;CHECK(pointers[index]);
            size_t expected_bytes=class_live_bytes-sizes[index],expected_live=live-1;
            counted_free(pointers[index]);pointers[index]=NULL;
            CHECK(live==expected_live && class_live_bytes==expected_bytes);
            if(i%53==0)allocation_bookkeeping_verify(pointers,sizes,identities,count);
        }
        allocation_bookkeeping_verify(pointers,sizes,identities,count);
    }
    CHECK(!live && !class_live_bytes && fail_at==SIZE_MAX);
    puts("Allocation bookkeeping: three real growth/move/churn/retirement rounds, failed malloc/calloc/realloc, independent pointer/byte/identity oracle passed");
}
#endif // XIR_ALLOCATION_BOOKKEEPING_CASES_H
