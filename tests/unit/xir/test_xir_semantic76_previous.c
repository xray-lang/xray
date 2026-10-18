/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_semantic76_previous.c - Complete previous semantic packets and untouched outputs
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_channel_types75_golden.h"
#include "xir_task_types75_golden.h"
#include "xir_task_unit75_golden.h"
#include "xir_task_go75_golden.h"
#include "xir_channel_blocking75_golden.h"
#include "xir_assert_panics75_golden.h"
int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==28 && XR_XIR_CHECKED_CONTRACT==76,"current semantic boundary");
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    const uint8_t *previous[]={channel_types75_golden,channel_unit75_golden,task_types75_golden,task_unit75_golden,task_go75_golden,task_go75_scalar_go,task_go75_initializer_target,task_go75_await_place,task_go75_go_target,task_go75_await_binding,task_go75_mutable_worker,task_go75_cleanup_helper,blocking_send_i6475_golden,blocking_send_unit75_golden,blocking_recv_or_i6475_golden,blocking_recv_or_unit75_golden,assert_panics75_golden};
    const size_t lengths[]={sizeof(channel_types75_golden),sizeof(channel_unit75_golden),sizeof(task_types75_golden),sizeof(task_unit75_golden),sizeof(task_go75_golden),sizeof(task_go75_scalar_go),sizeof(task_go75_initializer_target),sizeof(task_go75_await_place),sizeof(task_go75_go_target),sizeof(task_go75_await_binding),sizeof(task_go75_mutable_worker),sizeof(task_go75_cleanup_helper),sizeof(blocking_send_i6475_golden),sizeof(blocking_send_unit75_golden),sizeof(blocking_recv_or_i6475_golden),sizeof(blocking_recv_or_unit75_golden),sizeof(assert_panics75_golden)};
    for (unsigned i=0;i<17;++i) for (unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *sentinel=occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL,*output=sentinel;
        size_t attempts=source_program_compile_attempts;XrCompileResourceStats before=library_compile_stats(&owner.context);
        CHECK(xr_xir_compile_checked_read(&owner.context,previous[i],lengths[i],&output,NULL)==XR_XIR_BAD_STRUCTURE && output==sentinel);
        XrCompileResourceStats after=library_compile_stats(&owner.context);
        CHECK(source_program_compile_attempts==attempts && !runtime_live && !runtime_bytes && after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    }
    library_compile_owner_drop(&owner);library_compile_observer_free();puts("17 complete independent previous75 Task/Channel/blocking/Core packets: empty/occupied output and allocation ledger unchanged PASS");return 0;
}
