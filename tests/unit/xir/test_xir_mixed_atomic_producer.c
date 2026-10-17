/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_producer.c - Real Source-derived Atomic execution bodies
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Atomic producer %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
#include "xir_mixed_atomic_source.h"
#include "base/xsha256.h"
int main(int argc,char **argv) {
    CHECK(argc==2);AtomicSourcePrepare source={.context=effects_source_owner(67108864,128000000)};
    bool complete=atomic_source_prepare(&source);
    if(!complete)fprintf(stderr,"Atomic producer stage=%u status=%u\n",source.stage,(unsigned)source.status);
    CHECK(complete);const XrXirModule *module=xr_xir_compile_artifact_module(source.lowered);
    unsigned char digest[32];xr_sha256((const uint8_t *)source.generated.text,source.generated.length,digest);
    FILE *file=fopen(argv[1],"wb");CHECK(file);
    CHECK(fwrite(source.generated.text,1,source.generated.length,file)==source.generated.length);
    CHECK(fprintf(file,"\nconst uint32_t p20_atomic_runtime_entries[15]={")>0);
    for(unsigned i=0;i<ATOMIC_RUNTIME_ENTRIES;++i)CHECK(fprintf(file,"%s%uu",i?",":"",source.entries[i])>0);
    CHECK(fprintf(file,"};\nconst unsigned char p20_atomic_runtime_c_sha256[32]={")>0);
    for(unsigned i=0;i<32;++i)CHECK(fprintf(file,"%s%u",i?",":"",(unsigned)digest[i])>0);
    CHECK(fprintf(file,"};\nconst size_t p20_atomic_runtime_c_bytes=%zu;\n",source.generated.length)>0);
    CHECK(fclose(file)==0);
    printf("Atomic real Source modules=%u functions=%u slots=%u cbytes=%zu SourceSessionDead independentChecked poisonedPacket originalCompilerOwner\n",
        module->declarations->module_count,module->function_count,module->declarations->slot_count,source.generated.length);
    atomic_source_cleanup(&source);effects_source_owners_free();return 0;
}
