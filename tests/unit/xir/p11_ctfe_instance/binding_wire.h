/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * binding_wire.h - Complete populated literal packets constrain both codec directions
 *
 * KEY CONCEPT:
 *   Independent populated bytes constrain both codec directions and hostile inputs.
 */
#include "comptime_binding75_golden.h"
static void binding_wire(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*closed=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_checked_read(&context,
        comptime_binding75_golden,sizeof(comptime_binding75_golden),&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Binding independent packet=%u f=%u b=%u i=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(comptime_binding75_golden) && !memcmp(packet.bytes,comptime_binding75_golden,packet.length));
    xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    const uint32_t offsets[]={COMPTIME_BINDING75_SITE_COUNT,COMPTIME_BINDING75_FIRST_ARGUMENT,COMPTIME_BINDING75_FIRST_PROVIDER};
    const uint32_t values[]={UINT32_MAX,XR_XIR_TYPE_PARAMETER_BASE,0};
    for (uint32_t i=0;i<3;++i) {
        uint8_t raw[sizeof(comptime_binding75_golden)];memcpy(raw,comptime_binding75_golden,sizeof(raw));
        for (uint32_t b=0;b<4;++b) raw[offsets[i]+b]=(uint8_t)(values[i]>>(8*b));
        XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,raw,32);
        xr_sha256_update(&hash,raw+64,sizeof(raw)-64);xr_sha256_final(&hash,raw+32);checked=NULL;
        CHECK(xr_xir_compile_checked_read(&context,raw,sizeof(raw),&checked,NULL)!=XR_XIR_OK && !checked);
    }
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
