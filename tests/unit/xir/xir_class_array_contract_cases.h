/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_contract_cases.h - Canonical class-field capability revision
 *
 * KEY CONCEPT:
 *   Digest-valid old semantics reject before artifact allocation; wire and ABI
 *   layouts remain unchanged while the admitted field type set expands.
 */
_Static_assert(XR_XIR_CHECKED_SCHEMA==19 && XR_XIR_CHECKED_CONTRACT==50,
    "class Array fields use the new semantic contract on the existing schema");
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION==25 && XR_XIR_VALUE_ABI_VERSION==15 &&
    XR_XIR_CALL_ABI_VERSION==19,"class field values use the current carrier ABIs");
static uint8_t *class_array_old_contract(const uint8_t *bytes,size_t length,uint8_t revision) {
    C(length>=64 && bytes[8]==19 && !bytes[9] && !bytes[10] && !bytes[11]);
    C(bytes[12]==50 && !bytes[13] && !bytes[14] && !bytes[15]);
    C(revision==47 || revision==48 || revision==49);
    uint8_t *old=malloc(length);C(old);memcpy(old,bytes,length);old[12]=revision;
    XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,old,32);
    xr_sha256_update(&sha,old+64,length-64);xr_sha256_final(&sha,old+32);
    return old;
}
static void class_array_contract_packet(const uint8_t *bytes,size_t length) {
    XrXirArtifact *checked=NULL;XrXirCheckedPacket roundtrip={0};
    C(xr_xir_checked_read(bytes,length,NULL,&checked,NULL)==XR_XIR_OK);
    C(xr_xir_artifact_verify(checked,NULL,NULL)==XR_XIR_OK);
    C(xr_xir_checked_write(checked,NULL,&roundtrip,NULL)==XR_XIR_OK);
    C(roundtrip.length==length && !memcmp(roundtrip.bytes,bytes,length));
    xr_xir_checked_packet_free(&roundtrip);xr_xir_artifact_free(checked);
    for(uint8_t revision=47;revision<=49;++revision) {
    uint8_t *old=class_array_old_contract(bytes,length,revision);
    size_t before=runtime_attempts,live=runtime_live,physical=runtime_bytes;
    runtime_fail_at=before;checked=(XrXirArtifact *)(uintptr_t)1;
    XrXirDiagnostic diagnostic={0};
    C(xr_xir_checked_read(old,length,NULL,&checked,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    C(!checked && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    C(runtime_attempts==before && runtime_live==live && runtime_bytes==physical);
    runtime_fail_at=SIZE_MAX;free(old);
    }
}
