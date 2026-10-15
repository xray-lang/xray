static void owner_profile_relation_suite(void) {
    static const wchar_t *const left[]={L"C:\\root",L"C:\\root",L"C:\\ordinal\\é",L"C:\\root",L"C:\\root\\child",L"C:\\root"};
    static const wchar_t *const right[]={L"d:\\other",L"c:\\ROOT",L"c:\\ORDINAL\\É",L"c:\\ROOT\\child",L"c:\\ROOT",L"c:\\ROOTED"};
    static const int expected[]={3,0,0,1,2,3};
    for(unsigned row=0;row<6;++row) {
        size_t a=wcslen(left[row]),b=wcslen(right[row]),common=a<b?a:b;
        uint64_t fee=4*sizeof(wchar_t)+1;
        if(row)fee+=2*common*sizeof(wchar_t)+1+(a!=b?2*sizeof(wchar_t):0);
        for(unsigned less=0;less<2;++less) {
            XrCompileResourceLimits limits=sdk_unlimited;limits.work=1+fee-less;
            XrCompileResources *resources=sdk_ledger(&limits);
            XrXirInvocation owner={0};owner.context.resources=resources;int relation=99;
            CHECK(invocation_profile_relation(&owner,left[row],a,right[row],b,&relation)==!less);
            if(less)CHECK(owner.status==XR_XIR_INVOCATION_BUDGET);
            else CHECK(relation==expected[row]);
            XrCompileResourceStats used;CHECK(xr_compile_resources_stats(resources,&used)==XR_COMPILE_RESOURCE_OK);
            CHECK(used.work<=limits.work && (less||used.work==limits.work));
            xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
        }
    }
    puts("path relation: disjoint drive prefilter, same-drive Unicode/ancestor checks, exact-minus-one work PASS");
}
/* Constructed rows exercise only the private lookup index. No file lease,
 * Target, provider or execution authority is issued by these fixtures. */
static void owner_file_index_suite(void) {
    owner_profile_relation_suite();
    static const char first[]="C:/collision/8ca9549776dab8eb.h";
    static const char second[]="C:/collision/1e5641d0e81ef2d0.h";
    CHECK(xr_hash_bytes(first,sizeof(first)-1)==UINT32_C(3894133599));
    CHECK(xr_hash_bytes(second,sizeof(second)-1)==UINT32_C(3894133599));
    for(unsigned scenario=0;scenario<3;++scenario) {
        XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
        XrXirInvocation owner={0};owner.context.resources=resources;owner.limits.files=8;
        owner.limits.dependencies.path_bytes=32768;
        XtcXirFileFacts a={first,4,{1}},b={second,4,{2}};
        CHECK(invocation_file_publish(&owner,XR_XIR_INVOCATION_GENERATED,XR_XIR_INVOCATION_HEADER,&a,NULL));
        CHECK(invocation_file_publish(&owner,XR_XIR_INVOCATION_GENERATED,XR_XIR_INVOCATION_HEADER,&b,NULL));
        CHECK(owner.files[0].key==owner.files[1].key && owner.files[1].next==1);
        uint32_t index=99;
        if(!scenario) {
            CHECK(invocation_file_find(&owner,&a,0,XR_XIR_INVOCATION_HEADER,&index) && index==0);
            CHECK(invocation_file_find(&owner,&b,0,XR_XIR_INVOCATION_HEADER,&index) && index==1);
            CHECK(invocation_file_find(&owner,&a,1,XR_XIR_INVOCATION_HEADER,&index) && index==UINT32_MAX);
            a.digest[0]^=1;index=99;
            CHECK(!invocation_file_find(&owner,&a,0,XR_XIR_INVOCATION_HEADER,&index));
            CHECK(owner.status==XR_XIR_INVOCATION_REPLAY_MISMATCH && index==99);
        } else {
            if(scenario==1)owner.files[1].next=2;
            else owner.file_buckets[owner.files[1].key&(INVOCATION_FILE_BUCKET_COUNT-1)]=owner.capacity+1;
            CHECK(!invocation_file_find(&owner,&a,0,XR_XIR_INVOCATION_HEADER,&index));
            CHECK(owner.status==XR_XIR_INVOCATION_INVALID && index==99);
        }
        xr_compile_resources_free(owner.files);xr_compile_resources_release(resources);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    puts("lookup index: independent FNV collision, exact path/digest/stage checks, corrupt chain rejection, physical=0 PASS");
}
