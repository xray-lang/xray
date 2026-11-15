/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * An impossible metadata budget rejects before any receiving allocation. */
static void nominal_layout_name_budgets(void) {
    for(unsigned scenario=0;scenario<3;++scenario) {
        char text[4096];memset(text,'a',sizeof(text));
        XrtCallableParameterSpec field={.value={XRT_CALLABLE_SCALAR,XR_POD_I64},.name="score",.name_length=5};
        XrtCallableTypeSpec type={.kind=XRT_CALLABLE_RECORD,.module="nominal",.module_length=7,
            .name="Score",.name_length=5,.count=1,.parameters=&field,.parameter_count=1};
        if(scenario==0){type.module=text;type.module_length=sizeof(text);}
        if(scenario==1){type.name=text;type.name_length=sizeof(text);}
        if(scenario==2){field.name=text;field.name_length=sizeof(text);}
        XrtModuleSpec module={0};
        XrtModuleLayoutSpec spec={.metadata=XRT_MODULE_LAYOUT_HEADER,.modules=&module,.module_count=1,.payload_alignment=1,
            .callable_types=&type,.callable_type_count=1,.byte_limit=1024};
        XrtModuleLayout *layout=NULL;calls=0;fail_at=0;injected=false;
        XrtModuleStatus status=xrt_module_layout_new(&spec,&layout);
        if(status!=XR_XIR_VALUE_MEMORY_LIMIT)fprintf(stderr,"nominal name budget scenario %u status %u allocations %zu\n",scenario,(unsigned)status,calls);
        CHECK(status==XR_XIR_VALUE_MEMORY_LIMIT&&!layout&&!injected&&!calls&&!live);
        fail_at=SIZE_MAX;spec.byte_limit=UINT64_MAX;calls=0;
        OK(xrt_module_layout_new(&spec,&layout));size_t sites=calls;CHECK(sites);
        memset(text,0xa5,sizeof(text));XrtModuleLayoutSpec view={.metadata=XRT_MODULE_LAYOUT_HEADER};OK(xrt_module_layout_view(layout,&view));
        const XrtCallableTypeSpec *received=&view.callable_types[0];
        const char *owned=scenario==0?received->module:scenario==1?received->name:received->parameters[0].name;
        uint32_t length=scenario==0?received->module_length:scenario==1?received->name_length:received->parameters[0].name_length;
        CHECK(length==sizeof(text));for(uint32_t n=0;n<length;++n)CHECK(owned[n]=='a');
        xrt_module_layout_drop(&layout);CHECK(!live);memset(text,'a',sizeof(text));
        for(size_t point=0;point<sites;++point) {
            calls=0;fail_at=point;injected=false;
            CHECK(xrt_module_layout_new(&spec,&layout)==XR_XIR_VALUE_OOM&&!layout&&injected&&!live);
        }
        fail_at=SIZE_MAX;
    }
}
