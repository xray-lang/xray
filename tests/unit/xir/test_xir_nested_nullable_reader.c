/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nested_nullable_reader.c - Isolated historical/current reader probe
 *
 * KEY CONCEPT:
 *   A real unmodified reader is archived before the type admission change.
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
int main(int argc,char **argv){
    CHECK(argc==3);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    FILE *file=fopen(argv[1],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>=64 && length<=262144 && !fseek(file,0,SEEK_SET));
    uint8_t *bytes=NULL;CHECK(xr_compile_resources_alloc(owner.context.resources,(size_t)length,(void**)&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL;XrXirStatus status=xr_xir_compile_checked_read(&owner.context,bytes,(size_t)length,&checked,NULL);
    unsigned expected=(unsigned)strtoul(argv[2],NULL,10);CHECK((unsigned)status==expected);
    CHECK((checked!=NULL)==(status==XR_XIR_OK));xr_xir_compile_artifact_free(checked);xr_compile_resources_free(bytes);
    source_fixture_owner_free(&owner);printf("real Checked reader status=%u\n",(unsigned)status);return 0;
}
