/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_text_output_native.c - Real generated Rune and string typed output
 *
 * KEY CONCEPT:
 *   Native code executes the same checked source with independent UTF8 goldens.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xplatform.h"
#ifdef XR_OS_WINDOWS
#include <io.h>
#include <fcntl.h>
#endif
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_text_output_cases.h"
XR_DATA const XrXirProgramSpec text_output_program;
XR_DATA const uint32_t text_output_entry;
XR_DATA const uint32_t text_output_ids[7];
int main(void) {
#ifdef XR_OS_WINDOWS
    CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
    CHECK(text_output_entry<text_output_program.entry_count);
    CHECK(text_output_program.entries[text_output_entry].result==XR_XIR_I64 &&
        !text_output_program.entries[text_output_entry].parameter_count);
    const XrXirCompileContext *context=source_program_owner(UINT64_C(32)*1024*1024,UINT64_C(64000000));
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&text_output_program,&program)==XR_XIR_OK && program);
    CHECK(text_output_ids[0]==text_output_entry);
    text_output_pair(program,text_output_ids,true);source_program_owners_free();return 0;
}
