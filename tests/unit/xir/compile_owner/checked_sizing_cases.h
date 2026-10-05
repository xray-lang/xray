/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * checked_sizing_cases.h - Constant sizing and independent canonical byte evidence
 */
static const XrCompileResourceLimits sizing_caps = {67108864,8388608,128000000};
static const uint8_t sizing_golden[224] = {
    0x58,0x52,0x43,0x48,0x4b,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
    0x3f,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xa0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x16,0x14,0x4b,0x54,
    0x64,0x8f,0x47,0x10,0x51,0x3f,0x88,0xc8,0x80,0x13,0x51,0x48,
    0x17,0xa1,0x45,0x73,0x08,0x40,0x8d,0xc6,0xb4,0x31,0x0b,0x01,
    0xb4,0x1a,0x03,0x87,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x6d,0x61,0x69,0x6e,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2a,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x19,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t sizing_embedded_golden[224] = {
    0x58,0x52,0x43,0x48,0x4b,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
    0x3f,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xa0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x9d,0xbf,0x57,0x58,
    0x14,0x35,0x8b,0xf7,0x95,0xd8,0xc1,0x54,0x59,0x12,0x26,0xef,
    0xe3,0x2b,0x0b,0x49,0x53,0x17,0xb9,0x57,0xfe,0xb6,0x07,0x3d,
    0x1b,0xea,0x52,0x08,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x6d,0x00,0x69,0x6e,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2a,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x19,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t sizing_empty_golden[220] = {
    0x58,0x52,0x43,0x48,0x4b,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
    0x3f,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x9c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x98,0x74,0xef,0xf5,
    0xc3,0x57,0xe5,0x97,0x15,0xcb,0x63,0x7a,0xdd,0xba,0xc0,0xae,
    0x50,0xea,0x27,0x6d,0xf0,0xf9,0x90,0xe3,0xad,0x4b,0x96,0x3d,
    0xa1,0xb7,0xe4,0x85,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x2a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x19,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
};
static CheckedCursor sizing_cursor(XrCompileResources *owner, size_t position, size_t capacity) {
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    return (CheckedCursor){NULL,NULL,position,capacity,context.limits,context,XR_XIR_OK,false};
}
static void sizing_fields(void) {
    /* Two integers, two length fields and two blob advances are six operations. */
    const char *unreadable = (const char *)(uintptr_t)1;
    for (uint64_t work = 1; work <= 7; ++work) {
        reset(SIZE_MAX); XrCompileResources *owner = NULL;
        XrCompileResourceLimits limits = sizing_caps; limits.work = work;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CheckedCursor c = sizing_cursor(owner,0,SIZE_MAX);
        uint64_t first = checked_integer(&c,UINT64_C(0xfedcba98),4);
        uint64_t second = checked_integer(&c,UINT64_C(0x87654321fedcba98),8);
        uint32_t large = UINT32_MAX-32, empty = 0;
        const char *large_view = checked_blob(&c,unreadable,&large);
        const char *empty_view = checked_blob(&c,unreadable,&empty);
        CHECK(c.status == (work < 7 ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(stats(owner).work == work && attempts == 1 && live_count == 1);
        if (work == 7) {
            CHECK(first == UINT64_C(0xfedcba98) && second == UINT64_C(0x87654321fedcba98));
            CHECK(large_view == unreadable && empty_view == unreadable);
            CHECK(large == UINT32_MAX-32 && !empty && c.position == (size_t)UINT32_MAX-12);
        }
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
}
static void sizing_room_and_sticky(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&sizing_caps,&owner) == XR_COMPILE_RESOURCE_OK);
    CheckedCursor c = sizing_cursor(owner,SIZE_MAX-3,SIZE_MAX);
    CHECK(!checked_integer(&c,42,4) && c.status == XR_XIR_BUDGET);
    CHECK(c.position == SIZE_MAX-3 && stats(owner).work == 1);
    c = sizing_cursor(owner,SIZE_MAX-8,SIZE_MAX);
    CHECK(checked_integer(&c,42,8) == 42 && c.position == SIZE_MAX && c.status == XR_XIR_OK);
    CHECK(!checked_integer(&c,42,4) && c.position == SIZE_MAX && c.status == XR_XIR_BUDGET);
    CHECK(stats(owner).work == 2);
    c = sizing_cursor(owner,0,4);
    uint32_t length = 3;
    CHECK(!checked_blob(&c,(const char *)(uintptr_t)1,&length));
    CHECK(length == 3 && c.position == 4 && c.status == XR_XIR_BUDGET && stats(owner).work == 3);
    c = sizing_cursor(owner,1,2); c.status = XR_XIR_BAD_TYPE; c.reading = true;
    c.input = (const uint8_t *)(uintptr_t)1; c.output = (uint8_t *)(uintptr_t)1;
    length = UINT32_MAX;
    CHECK(!checked_integer(&c,42,8) && !checked_blob(&c,(const char *)(uintptr_t)1,&length));
    CHECK(c.status == XR_XIR_BAD_TYPE && c.position == 1 && !length && stats(owner).work == 3);
    c = sizing_cursor(owner,SIZE_MAX-3,SIZE_MAX); c.reading = true;
    c.input = (const uint8_t *)(uintptr_t)1;
    CHECK(!checked_integer(&c,0,4) && c.status == XR_XIR_BAD_STRUCTURE && c.position == SIZE_MAX-3);
    CHECK(stats(owner).work == 3 && live_count == 1);
    c = sizing_cursor(owner,0,4); uint32_t remaining = 2;
    CHECK(!checked_count(&c,3,&remaining) && c.status == XR_XIR_BUDGET && remaining == 2);
    CHECK(c.position == 4 && stats(owner).work == 4);
    CHECK(xr_compile_resources_work(owner,sizing_caps.work-4) == XR_COMPILE_RESOURCE_OK);
    c = sizing_cursor(owner,0,8); c.reading = true; c.input = (const uint8_t *)(uintptr_t)1;
    CHECK(!checked_integer(&c,0,8) && c.status == XR_XIR_BUDGET && !c.position);
    length = UINT32_MAX;
    CHECK(!checked_blob(&c,(const char *)(uintptr_t)1,&length) && !length);
    CHECK(c.status == XR_XIR_BUDGET && !c.position && stats(owner).work == sizing_caps.work);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
static void sizing_byte_fields(void) {
    static const uint8_t golden[] = {0x98,0xba,0xdc,0xfe,0x98,0xba,0xdc,0xfe,
        0x21,0x43,0x65,0x87,3,0,0,0,'a',0,'b'};
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&sizing_caps,&owner) == XR_COMPILE_RESOURCE_OK);
    uint8_t bytes[sizeof(golden)]; memset(bytes,0xcc,sizeof(bytes));
    CheckedCursor c = sizing_cursor(owner,0,sizeof(bytes)); c.output = bytes;
    CHECK(checked_integer(&c,UINT64_C(0xfedcba98),4) == UINT64_C(0xfedcba98));
    CHECK(checked_integer(&c,UINT64_C(0x87654321fedcba98),8) == UINT64_C(0x87654321fedcba98));
    const char payload[] = {'a',0,'b'}; uint32_t length = 3;
    CHECK(checked_blob(&c,payload,&length) == payload && c.status == XR_XIR_OK);
    CHECK(c.position == sizeof(golden) && !memcmp(bytes,golden,sizeof(golden)));
    CHECK(stats(owner).work == 20 && attempts == 1);
    c = sizing_cursor(owner,0,sizeof(golden)); c.reading = true; c.input = golden;
    CHECK(checked_integer(&c,0,4) == UINT64_C(0xfedcba98));
    CHECK(checked_integer(&c,0,8) == UINT64_C(0x87654321fedcba98));
    length = 0; const char *copy = checked_blob(&c,NULL,&length);
    CHECK(copy && copy != payload && length == 3 && !memcmp(copy,payload,3));
    CHECK(c.status == XR_XIR_OK && c.position == sizeof(golden));
    /* Reader: nineteen byte accesses plus one allocation and three cleared bytes. */
    CHECK(stats(owner).work == 43 && attempts == 2);
    xr_compile_resources_free((void *)copy); CHECK(live_count == 1);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
static XrXirStatus sizing_packet(XrCompileResources *owner, unsigned variant) {
    static const char embedded[] = {'m',0,'i','n'};
    XrXirFunction f = function;
    if (variant == 1) f.name = embedded;
    else if (variant == 2) { f.name = (const char *)(uintptr_t)1; f.name_length = 0; }
    XrXirArtifact artifact = {0}; artifact.module = module; artifact.module.stage = XR_XIR_CHECKED;
    artifact.module.functions = &f;
    artifact.context = (XrXirCompileContext){owner,xr_xir_compile_default_limits()};
    uint8_t sentinel = 0x5a; XrXirCheckedPacket packet = {&sentinel,17};
    XrXirStatus status = checked_encode(&artifact,&packet,NULL);
    if (status != XR_XIR_OK) CHECK(packet.bytes == &sentinel && packet.length == 17 && sentinel == 0x5a);
    else {
        CHECK(packet.bytes != &sentinel && packet.length == (variant == 2 ? 220 : 224));
        if (!variant) CHECK(!memcmp(packet.bytes,sizing_golden,sizeof(sizing_golden)));
        else if (variant == 1) CHECK(!memcmp(packet.bytes,sizing_embedded_golden,sizeof(sizing_embedded_golden)));
        else CHECK(!memcmp(packet.bytes,sizing_empty_golden,sizeof(sizing_empty_golden)));
        xr_xir_compile_checked_packet_free(&packet);
    }
    CHECK(live_count == 1); return status;
}
static bool sizing_oracle_charge(uint64_t limit, uint64_t charge, uint64_t *spent) {
    if (charge > limit-*spent) return false;
    *spent += charge; return true;
}
static uint64_t sizing_prefix_work(uint64_t limit, uint64_t size) {
    /* Header fields, body fields, calloc and digest are atomic real operations. */
    static const uint64_t header[] = {8,4,4,4,4,8};
    static const uint64_t body[] = {4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,8,4,4,4,4,4,4,4,4,8,4,4,4,4,4,4,4,4,4};
    uint64_t spent = 1;
    for (unsigned i = 0; i < 38; ++i) if (!sizing_oracle_charge(limit,1,&spent)) return spent;
    if (!sizing_oracle_charge(limit,size+1,&spent)) return spent;
    for (size_t i = 0; i < sizeof(header)/sizeof(header[0]); ++i)
        if (!sizing_oracle_charge(limit,header[i],&spent)) return spent;
    for (size_t i = 0; i < sizeof(body)/sizeof(body[0]); ++i)
        if (!sizing_oracle_charge(limit,i == 4 ? size-220 : body[i],&spent)) return spent;
    (void)sizing_oracle_charge(limit,size-32,&spent);
    return spent;
}
static void sizing_packet_boundaries(unsigned variant) {
    /* Thirty-seven integer fields plus a blob advance. Wire body is 156+name bytes. */
    const uint64_t size = variant == 2 ? 220 : 224;
    const uint64_t work = 1 + 38 + (size+1) + (size-32) + (size-32);
    const uint64_t storage = sizeof(XrCompileResources) + sizeof(CompileAllocation) + size;
    for (uint64_t cut = 1; cut <= work; ++cut) {
        reset(SIZE_MAX); XrCompileResources *owner = NULL;
        XrCompileResourceLimits limits = sizing_caps; limits.work = cut;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(sizing_packet(owner,variant) == (cut < work ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(stats(owner).work == sizing_prefix_work(cut,size));
        if (cut == work) CHECK(stats(owner).work == work && stats(owner).allocated_bytes == storage && attempts == 2);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    for (unsigned axis = 0; axis < 2; ++axis) for (unsigned below = 0; below < 2; ++below) {
        reset(SIZE_MAX); XrCompileResources *owner = NULL; XrCompileResourceLimits limits = sizing_caps;
        if (!axis) limits.allocated_bytes = storage-below; else limits.live_bytes = storage-below;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(sizing_packet(owner,variant) == (below ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(stats(owner).peak_bytes <= limits.live_bytes);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    for (size_t failure = 0; failure <= 1; ++failure) {
        reset(failure); XrCompileResources *owner = NULL;
        XrCompileResourceStatus status = xr_compile_resources_new(&sizing_caps,&owner);
        if (!failure) CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !owner);
        else {
            CHECK(status == XR_COMPILE_RESOURCE_OK);
            CHECK(sizing_packet(owner,variant) == XR_XIR_OUT_OF_MEMORY && attempts == 2);
            CHECK(stats(owner).work == 40); xr_compile_resources_release(owner);
        }
        CHECK(!live && !live_count);
    }
}
static void sizing_public_golden(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&sizing_caps,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirArtifact *checked = NULL, *decoded = NULL; XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_check(&context,&module,&checked,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(sizing_golden) && !memcmp(packet.bytes,sizing_golden,sizeof(sizing_golden)));
    CHECK(xr_xir_compile_checked_read(&context,sizing_golden,sizeof(sizing_golden),&decoded,NULL) == XR_XIR_OK);
    CHECK(decoded->module.functions[0].instructions[0].immediate == 42);
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); CHECK(live_count == 1);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
static void checked_sizing_cases(void) {
    sizing_fields(); sizing_room_and_sticky(); sizing_byte_fields();
    for (unsigned variant = 0; variant < 3; ++variant) sizing_packet_boundaries(variant);
    sizing_public_golden();
    puts("Checked sizing: constant fields, fixed byte KAT, all work prefixes, axes/OOM and physical zero");
}
