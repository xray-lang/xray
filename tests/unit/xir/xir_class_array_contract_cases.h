/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_contract_cases.h - Canonical class-field capability revision
 *
 * KEY CONCEPT:
 *   Digest-valid old semantics reject before artifact allocation. Class field
 *   values use the same owned failure carrier as every other runtime value.
 */
_Static_assert(XR_XIR_CHECKED_SCHEMA==24 && XR_XIR_CHECKED_CONTRACT==63,
    "class Array fields use the current default-binding schema and semantic contract");
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION==28 && XR_XIR_VALUE_ABI_VERSION==20 &&
    XR_XIR_CALL_ABI_VERSION==25,"class field values use the current carrier ABIs");
static uint8_t *class_array_old_contract(const uint8_t *bytes,size_t length,uint8_t revision) {
    C(length>=64 && bytes[8]==24 && !bytes[9] && !bytes[10] && !bytes[11]);
    C(bytes[12]==63 && !bytes[13] && !bytes[14] && !bytes[15]);
    C(revision>=47 && revision<=59);
    uint8_t *old=malloc(length);C(old);memcpy(old,bytes,length);old[12]=revision;
    old[8]=revision==59?22:23;
    XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,old,32);
    xr_sha256_update(&sha,old+64,length-64);xr_sha256_final(&sha,old+32);
    return old;
}
static void class_array_contract_packet(const XrXirCompileContext *context,const uint8_t *bytes,size_t length) {
    XrXirArtifact *checked=NULL;XrXirCheckedPacket roundtrip={0};
    C(xr_xir_compile_checked_read(context,bytes,length,&checked,NULL)==XR_XIR_OK);
    C(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    C(xr_xir_compile_checked_write(checked,&roundtrip,NULL)==XR_XIR_OK);
    C(roundtrip.length==length && !memcmp(roundtrip.bytes,bytes,length));
    xr_xir_compile_checked_packet_free(&roundtrip);xr_xir_compile_artifact_free(checked);
    for(uint8_t revision=47;revision<=59;++revision) {
    uint8_t *old=class_array_old_contract(bytes,length,revision);
    size_t before=instance_compile_attempts,live=instance_compile_live,physical=instance_compile_bytes;
    instance_compile_fail_at=before;checked=NULL;
    XrXirDiagnostic diagnostic={0};
    C(xr_xir_compile_checked_read(context,old,length,&checked,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    C(!checked && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    C(instance_compile_attempts==before && instance_compile_live==live && instance_compile_bytes==physical);
    checked=(XrXirArtifact *)(uintptr_t)1;
    C(xr_xir_compile_checked_read(context,old,length,&checked,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    C(checked==(XrXirArtifact *)(uintptr_t)1 && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    C(instance_compile_attempts==before && instance_compile_live==live && instance_compile_bytes==physical);
    instance_compile_fail_at=SIZE_MAX;free(old);
    }
}
