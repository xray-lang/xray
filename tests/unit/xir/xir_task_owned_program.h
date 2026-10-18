/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_owned_program.h - Unique native fixture inputs and atomic reception
 *
 * KEY CONCEPT:
 *   Native callback metadata enters the same receiver without a wire rebuild.
 */
#ifndef XIR_TASK_OWNED_PROGRAM_H
#define XIR_TASK_OWNED_PROGRAM_H
#include "xir/xxir_program_internal.h"
static bool task_receive_probe;
static unsigned task_receive_checks;
static size_t task_publication_attempts,task_publication_live,task_publication_bytes;
static XrXirStatus task_owned_program_take(const XrXirCompileContext *context,
    XrXirProgramSpec *spec,XrXirArtifact **artifact,XrXirProgram **output) {
    CHECK(spec && artifact && *artifact && output && !*output && !spec->packet.bytes &&
        !spec->packet.length && !spec->packet.identity);
    XrXirArtifact *original=*artifact,*owned=original;
    XrXirProgramInput *input=NULL;
    XrXirStatus status=xir_program_input_take(context,&owned,&input);
    if(status!=XR_XIR_OK)return status;
    if(task_receive_probe){
        XrXirProgramInput *retained=input;
        TaskWitness *witness=spec->code.owner;
        uint32_t abi=spec->abi_version, releases=witness->code_releases;
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        spec->abi_version=0;
        CHECK(xir_program_receive(context,spec,&input,NULL,output)==XR_XIR_BAD_LAYOUT);
        CHECK(input==retained && input->artifact==original && !*output &&
            *artifact==original && witness->code_releases==releases);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.allocated_bytes==before.allocated_bytes &&
            after.work>=before.work);
        spec->abi_version=abi;++task_receive_checks;
    }
    status=xir_program_receive(context,spec,&input,NULL,output);
    if(status==XR_XIR_OK){
        CHECK(!input && *output);*artifact=NULL;
        if(task_receive_probe){
            task_publication_attempts=runtime_attempts;
            task_publication_live=runtime_live;task_publication_bytes=runtime_bytes;
        }
    }
    else {input->artifact=NULL;xir_program_input_free(input);}
    return status;
}
#endif // XIR_TASK_OWNED_PROGRAM_H
