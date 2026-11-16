/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_literal.h - Strict explicit entries and ordered spread transactions
 */
static void hash_literal_display(XrXirValueAdmission *admission) {
    XrXirValue keys[]={integer(1),integer(2)},values[]={integer(10),integer(20)},map={0},text={0};
    XrXirMapLiteralDuplicate duplicate={0};
    CHECK(xr_xir_map_literal_new((XrXirType)256,keys,values,NULL,2,admission,&map,&duplicate)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_display_text(&map,admission,&text)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;
    static const char expected[]="{1: 10, 2: 20}";
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    xr_xir_value_drop(&map);xr_xir_value_drop(&text);
    XrXirValue a={0},b={0},set={0};
    CHECK(xr_xir_string_new(admission->domain,"a",1,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(admission->domain,"b",1,&b)==XR_XIR_VALUE_OK);
    XrXirValue elements[]={a,a,b};
    CHECK(xr_xir_set_new((XrXirType)262,elements,3,admission,&set)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&a);xr_xir_value_drop(&b);
    CHECK(xr_xir_value_display_text(&set,admission,&text)==XR_XIR_VALUE_OK);
    static const char set_expected[]="Set([\"a\", \"b\"])";
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==sizeof(set_expected)-1 && !memcmp(bytes,set_expected,length));
    xr_xir_value_drop(&set);xr_xir_value_drop(&text);
}
static void hash_literal_cases(XrXirValueAdmission *admission) {
    XrXirValue keys[]={integer(1),integer(2),integer(3)},values[]={integer(10),integer(20),integer(30)};
    XrXirValue map={0};XrXirMapLiteralDuplicate duplicate={77,88};
    uint64_t baseline=xr_xir_domain_stats(admission->domain).live_bytes;
    uint64_t work=admission->work;size_t allocations=value_allocation_calls;
    CHECK(xr_xir_map_new((XrXirType)256,keys,values,3,admission,&map)==XR_XIR_VALUE_OK);
    uint64_t ordinary_work=work-admission->work;size_t ordinary_allocations=value_allocation_calls-allocations;
    uint64_t ordinary_bytes=xr_xir_domain_stats(admission->domain).live_bytes-baseline;
    xr_xir_value_drop(&map);work=admission->work;allocations=value_allocation_calls;
    CHECK(xr_xir_map_literal_new((XrXirType)256,keys,values,NULL,3,admission,&map,&duplicate)==XR_XIR_VALUE_OK);
    CHECK(work-admission->work<=ordinary_work && value_allocation_calls-allocations==ordinary_allocations &&
        xr_xir_domain_stats(admission->domain).live_bytes-baseline==ordinary_bytes && duplicate.first==77 && duplicate.second==88);
    xr_xir_value_drop(&map);keys[2]=keys[0];
    CHECK(xr_xir_map_literal_new((XrXirType)256,keys,values,NULL,3,admission,&map,&duplicate)==XR_XIR_VALUE_DUPLICATE_KEY);
    CHECK(!map.type && !map.payload && duplicate.first==0 && duplicate.second==2 &&
        xr_xir_domain_stats(admission->domain).live_bytes==baseline);
    CHECK(xr_xir_map_new((XrXirType)256,keys,values,3,admission,&map)==XR_XIR_VALUE_OK);
    hash_expect(&map,1,30,true,admission);xr_xir_value_drop(&map);
    XrXirValue source={0};keys[2]=integer(3);
    CHECK(xr_xir_map_new((XrXirType)256,keys,values,3,admission,&source)==XR_XIR_VALUE_OK);
    XrXirValue spread_keys[]={{0},integer(1),{0},integer(2),integer(1)};
    XrXirValue spread_values[]={source,integer(90),source,integer(70),integer(80)};
    XrXirMapLiteralKind kinds[]={XR_XIR_MAP_LITERAL_SPREAD,XR_XIR_MAP_LITERAL_ENTRY,XR_XIR_MAP_LITERAL_SPREAD,
        XR_XIR_MAP_LITERAL_ENTRY,XR_XIR_MAP_LITERAL_ENTRY};
    baseline=xr_xir_domain_stats(admission->domain).live_bytes;
    CHECK(xr_xir_map_literal_new((XrXirType)256,spread_keys,spread_values,kinds,4,admission,&map,&duplicate)==XR_XIR_VALUE_OK);
    hash_expect(&map,1,10,true,admission);hash_expect(&map,2,70,true,admission);hash_expect(&map,3,30,true,admission);
    hash_expect(&source,1,10,true,admission);hash_expect(&source,2,20,true,admission);
    xr_xir_value_drop(&map);
    CHECK(xr_xir_map_literal_new((XrXirType)256,spread_keys,spread_values,kinds,5,admission,&map,&duplicate)==XR_XIR_VALUE_DUPLICATE_KEY);
    CHECK(duplicate.first==1 && duplicate.second==4 && !map.type && !map.payload &&
        xr_xir_domain_stats(admission->domain).live_bytes==baseline);
    for(unsigned malformed=0;malformed<3;++malformed) {
        XrXirMapLiteralKind saved=kinds[0];XrXirValue key=spread_keys[0],value=spread_values[0];
        if(malformed==0)kinds[0]=(XrXirMapLiteralKind)99;
        if(malformed==1)spread_keys[0]=integer(1);
        if(malformed==2)spread_values[0]=integer(1);
        duplicate=(XrXirMapLiteralDuplicate){77,88};
        CHECK(xr_xir_map_literal_new((XrXirType)256,spread_keys,spread_values,kinds,4,admission,&map,&duplicate)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!map.type && duplicate.first==77 && duplicate.second==88 && xr_xir_domain_stats(admission->domain).live_bytes==baseline);
        kinds[0]=saved;spread_keys[0]=key;spread_values[0]=value;
    }
    allocations=value_allocation_calls;work=admission->work;
    CHECK(xr_xir_map_literal_new((XrXirType)256,spread_keys,spread_values,kinds,4,admission,&map,&duplicate)==XR_XIR_VALUE_OK);
    size_t sites=value_allocation_calls-allocations;uint64_t exact=work-admission->work;
    xr_xir_value_drop(&map);CHECK(sites>0 && exact>0);
    for(size_t point=0;point<sites;++point) {
        duplicate=(XrXirMapLiteralDuplicate){77,88};fail_at=value_allocation_calls+point;
        admission->work=1000000;
        CHECK(xr_xir_map_literal_new((XrXirType)256,spread_keys,spread_values,kinds,4,admission,&map,&duplicate)==XR_XIR_VALUE_OOM);
        fail_at=SIZE_MAX;
        CHECK(!map.type && !map.payload && duplicate.first==77 && duplicate.second==88 &&
            xr_xir_domain_stats(admission->domain).live_bytes==baseline);
    }
    for(uint64_t prefix=0;prefix<=exact;++prefix) {
        admission->work=prefix;duplicate=(XrXirMapLiteralDuplicate){77,88};
        CHECK(xr_xir_map_literal_new((XrXirType)256,spread_keys,spread_values,kinds,4,admission,&map,&duplicate)==
            (prefix==exact?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        if(prefix<exact)CHECK(!map.type && !map.payload && duplicate.first==77 && duplicate.second==88);
        xr_xir_value_drop(&map);
        CHECK(xr_xir_domain_stats(admission->domain).live_bytes==baseline);
    }
    admission->work=100000000;xr_xir_value_drop(&source);
    XrXirValue strings[2]={{0}},elements[2]={{0}};
    for(unsigned n=0;n<2;++n) {
        CHECK(xr_xir_string_new(admission->domain,"a\0b",3,&strings[n])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(admission->domain,"value",5,&elements[n])==XR_XIR_VALUE_OK);
    }
    baseline=xr_xir_domain_stats(admission->domain).live_bytes;
    CHECK(xr_xir_map_literal_new((XrXirType)258,strings,elements,NULL,2,admission,&map,&duplicate)==XR_XIR_VALUE_DUPLICATE_KEY);
    CHECK(duplicate.first==0 && duplicate.second==1 && !map.type && xr_xir_domain_stats(admission->domain).live_bytes==baseline);
    for(unsigned n=0;n<2;++n){xr_xir_value_drop(&strings[n]);xr_xir_value_drop(&elements[n]);}
    printf("Strict Map literals: bounded pure work and unchanged allocations, ordered spread history, %zu allocation sites and all %llu work prefixes PASS\n",
        sites,(unsigned long long)exact);
}
static void hash_literal_foreign_strings(XrXirValueAdmission *admission) {
    XrXirDomain *producer=NULL;CHECK(xr_xir_domain_new(65536,&producer)==XR_XIR_VALUE_OK);
    XrXirValue key={0},value={0},map={0},query={0},found_value={0};
    CHECK(xr_xir_string_new(producer,"key\0full",8,&key)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(producer,"kept\0value",10,&value)==XR_XIR_VALUE_OK);
    XrXirMapLiteralDuplicate duplicate={77,88};
    CHECK(xr_xir_map_literal_new((XrXirType)258,&key,&value,NULL,1,admission,&map,&duplicate)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&key);xr_xir_value_drop(&value);
    xr_xir_domain_close(producer);xr_xir_domain_drop(producer);
    CHECK(xr_xir_string_new(admission->domain,"key\0full",8,&query)==XR_XIR_VALUE_OK);
    bool found=false;
    CHECK(xr_xir_map_get(&map,&query,admission,&found_value,&found)==XR_XIR_VALUE_OK && found);
    xr_xir_value_drop(&map);xr_xir_value_drop(&query);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&found_value,&bytes,&length) && length==10 && !memcmp(bytes,"kept\0value",10));
    xr_xir_value_drop(&found_value);
}
