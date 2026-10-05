/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_source_admission_owner.c - Governed Source Atomic and Ordering admission boundaries
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "shared/xnative_declaration.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
static char root_path[XR_TEST_PATH_MAX],directory_path[XR_TEST_PATH_MAX];
static void write_source(const char *source){FILE *f=fopen(root_path,"wb");CHECK(f);size_t n=strlen(source);CHECK(fwrite(source,1,n,f)==n && fclose(f)==0);}
#include "xir_atomic_source_admission_cases.h"
#include "xir_atomic_ordering_discovery_cases.h"
int main(void){
    char relative[XR_TEST_PATH_MAX]="atomic-source-XXXXXX",hidden[XR_TEST_PATH_MAX];CHECK(xr_test_mkdtemp(relative));
    CHECK(xr_test_realpath_buf(relative,directory_path,sizeof(directory_path)));
    CHECK(snprintf(root_path,sizeof(root_path),"%s/main.xr",directory_path)>0);
    CHECK(snprintf(hidden,sizeof(hidden),"%s/hidden.xr",directory_path)>0);
    FILE *f=fopen(hidden,"wb");CHECK(f);CHECK(fputs("enum Hidden { V }\nexport enum Fake { V }\n",f)>=0 && fclose(f)==0);
    atomic_source_controls();atomic_ordering_discovery_controls();CHECK(remove(root_path)==0 && remove(hidden)==0 && xr_test_rmdir(directory_path)==0);library_compile_observer_free();return 0;
}
