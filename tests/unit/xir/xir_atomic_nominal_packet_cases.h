/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_nominal_packet_cases.h - Independent governed packet rejection roles
 */
#ifndef XIR_ATOMIC_NOMINAL_PACKET_CASES_H
#define XIR_ATOMIC_NOMINAL_PACKET_CASES_H
static void ordering_packet_digest(uint8_t *bytes,size_t length) {
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bytes,32);
    xr_sha256_update(&hash,bytes+64,length-64);xr_sha256_final(&hash,bytes+32);
}
static void ordering_packet_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *sentinel=NULL;CHECK(xr_xir_compile_checked_read(&owner.context,ordering25_golden,
        sizeof(ordering25_golden),&sentinel,NULL)==XR_XIR_OK && sentinel);
    XrCompileResourceStats baseline=library_compile_stats(&owner.context);
    XrXirArtifact *empty=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,generic_method_golden,
        sizeof(generic_method_golden),&empty,NULL)==XR_XIR_BAD_STRUCTURE && !empty);
    for(unsigned mutation=0;mutation<7;++mutation){
        uint8_t bytes[sizeof(ordering25_golden)];memcpy(bytes,ordering25_golden,sizeof(bytes));
        switch(mutation){case 0:bytes[8]=24;bytes[12]=63;break;
        case 1:bytes[ORDERING25_RECORD_OFFSET]=0;break;
        case 2:bytes[ORDERING25_RECORD_OFFSET]=3;break;
        case 3:bytes[ORDERING25_RECORD_OFFSET+4]^=1;break;
        case 4:bytes[ORDERING25_VARIANT0_OFFSET+4]='X';break;
        case 5:bytes[ORDERING25_VARIANT0_OFFSET+4+7]=1;break;
        default:bytes[ORDERING25_RECORD_OFFSET+36+8]=4;break;}
        ordering_packet_digest(bytes,sizeof(bytes));XrXirArtifact *occupied=sentinel;
        CHECK(xr_xir_compile_checked_read(&owner.context,bytes,sizeof(bytes),&empty,NULL)==XR_XIR_BAD_STRUCTURE && !empty);
        CHECK(xr_xir_compile_checked_read(&owner.context,bytes,sizeof(bytes),&occupied,NULL)==XR_XIR_BAD_STRUCTURE && occupied==sentinel);
        CHECK(library_compile_stats(&owner.context).live_bytes==baseline.live_bytes);
    }
    xr_xir_compile_artifact_free(sentinel);library_compile_owner_drop(&owner);
}
static void ordering_float_constant_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirInstruction ops[2]={{.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    XrXirBlock block={.first=0,.count=2};XrXirFunction function={.name="float_literal",.name_length=13,
        .result=XR_XIR_F64,.blocks=&block,.block_count=1,.instructions=ops,.instruction_count=2};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.linkage_kind=XR_XIR_PROGRAM};
    const uint64_t payloads[]={UINT64_C(0x7ff8000000000000),UINT64_C(0x7ff8000000000001),
        UINT64_C(0x7ff0000000000001),UINT64_C(0xfff8000000000001),UINT64_C(0x8000000000000000)};
    for(unsigned i=0;i<sizeof(payloads)/sizeof(payloads[0]);++i){
        int64_t payload;memcpy(&payload,&payloads[i],sizeof(payload));ops[0].immediate=payload;
        CHECK(xr_xir_float_payload_valid(XR_XIR_F64,payload));
        bool valid=i==0||i==4;CHECK(xr_xir_float_constant_payload_valid(XR_XIR_F64,payload)==valid);
        XrXirArtifact *checked=NULL;XrXirStatus status=xr_xir_compile_check(&owner.context,&module,&checked,NULL);
        if(status!=(valid?XR_XIR_OK:XR_XIR_BAD_TYPE))fprintf(stderr,"float literal %u status%u valid%u\n",i,status,valid);
        CHECK(status==(valid?XR_XIR_OK:XR_XIR_BAD_TYPE));
        if(valid)CHECK(checked);else CHECK(!checked);
        if(i==0){XrXirCheckedPacket packet={0};
            CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
            uint8_t bytes[1024];CHECK(packet.length>157 && packet.length<=sizeof(bytes));
            CHECK(packet.bytes[149]==0 && packet.bytes[155]==0xf8 && packet.bytes[156]==0x7f);
            memcpy(bytes,packet.bytes,packet.length);bytes[149]=1;
            ordering_packet_digest(bytes,packet.length);XrXirArtifact *empty=NULL,*occupied=checked;
            CHECK(xr_xir_compile_checked_read(&owner.context,bytes,packet.length,&empty,NULL)==XR_XIR_BAD_TYPE && !empty);
            CHECK(xr_xir_compile_checked_read(&owner.context,bytes,packet.length,&occupied,NULL)==XR_XIR_BAD_TYPE && occupied==checked);
            xr_xir_compile_checked_packet_free(&packet);
        }
        xr_xir_compile_artifact_free(checked);
        CHECK(library_compile_stats(&owner.context).live_bytes==owner.baseline.live_bytes);
    }
    library_compile_owner_drop(&owner);
}
#endif // XIR_ATOMIC_NOMINAL_PACKET_CASES_H
