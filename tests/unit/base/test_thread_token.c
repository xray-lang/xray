/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_thread_token.c - Real thread lifetime identities and process owner cleanup
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "os/os_thread_token.h"
#include "base/xmalloc.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"thread token %d: %s\n",__LINE__,#condition);exit(1); \
} } while(0)

static size_t allocation_attempts, process_live;
static _Atomic(bool) fail_allocate, fail_close;
static _Atomic(bool) *cancel_allocate;
static unsigned close_faults;
static void *observed_calloc(size_t count,size_t size) {
    ++allocation_attempts;
    if (atomic_exchange_explicit(&fail_allocate,false,memory_order_acq_rel)) return NULL;
    void *memory=xr_calloc(count,size);
    if(memory)++process_live;
    if(cancel_allocate)atomic_store_explicit(cancel_allocate,false,memory_order_release);
    return memory;
}
static void observed_free(void *memory) {
    if(memory){CHECK(process_live);--process_live;}
    xr_free(memory);
}
#undef xr_calloc
#undef xr_free

#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static BOOL WINAPI observed_close(HANDLE handle) {
    if(atomic_exchange_explicit(&fail_close,false,memory_order_acq_rel)) {
        ++close_faults;SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    return CloseHandle(handle);
}
#define xr_calloc observed_calloc
#define xr_free observed_free
#define CloseHandle observed_close
#include "os/win/thread_token_win.c"
#undef CloseHandle
#undef xr_free
#undef xr_calloc
#else
#include <pthread.h>
#include <errno.h>
#include <time.h>
static int observed_key_delete(pthread_key_t key) {
    if(atomic_exchange_explicit(&fail_close,false,memory_order_acq_rel)) {++close_faults;return EIO;}
    return pthread_key_delete(key);
}
#define xr_calloc observed_calloc
#define xr_free observed_free
#define pthread_key_delete observed_key_delete
#include "os/unix/thread_token_unix.c"
#undef pthread_key_delete
#undef xr_free
#undef xr_calloc
#endif

typedef struct TokenThread {
#if defined(XR_OS_WINDOWS)
    HANDLE handle;
#else
    pthread_t handle;
#endif
    _Atomic(bool) ready,release,current;
    bool controlled;
    int64_t token,again;
    XrOsIoStatus status;
} TokenThread;
static bool controlled_current(void *context) {
    return atomic_load_explicit((_Atomic(bool) *)context,memory_order_acquire);
}
static uint64_t test_now(void) {
#if defined(XR_OS_WINDOWS)
    return GetTickCount64();
#else
    struct timespec value;CHECK(clock_gettime(CLOCK_MONOTONIC,&value)==0);
    return (uint64_t)value.tv_sec*1000+(uint64_t)value.tv_nsec/1000000;
#endif
}
static void test_pause(void) {
#if defined(XR_OS_WINDOWS)
    Sleep(1);
#else
    struct timespec value={0,1000000};(void)nanosleep(&value,NULL);
#endif
}
static void token_thread_body(TokenThread *thread) {
    XrOsThreadTokenQuery query={&thread->current,controlled_current};
    thread->status=xr_os_io_thread_token_current(thread->controlled?&query:NULL,&thread->token);
    if(thread->status==XR_OS_IO_OK)
        thread->status=xr_os_io_thread_token_current(NULL,&thread->again);
    atomic_store_explicit(&thread->ready,true,memory_order_release);
    while(!atomic_load_explicit(&thread->release,memory_order_acquire))test_pause();
}
#if defined(XR_OS_WINDOWS)
static DWORD WINAPI token_thread_entry(LPVOID context) {token_thread_body(context);return 0;}
#else
static void *token_thread_entry(void *context) {token_thread_body(context);return NULL;}
#endif
static void token_thread_start(TokenThread *thread,bool controlled) {
    memset(thread,0,sizeof(*thread));thread->controlled=controlled;thread->token=thread->again=-17;
    atomic_init(&thread->ready,false);atomic_init(&thread->release,false);atomic_init(&thread->current,true);
#if defined(XR_OS_WINDOWS)
    thread->handle=CreateThread(NULL,0,token_thread_entry,thread,0,NULL);CHECK(thread->handle);
#else
    CHECK(pthread_create(&thread->handle,NULL,token_thread_entry,thread)==0);
#endif
    uint64_t deadline=test_now()+10000;
    while(!atomic_load_explicit(&thread->ready,memory_order_acquire)) {CHECK(test_now()<deadline);test_pause();}
}
static void token_thread_join(TokenThread *thread) {
    atomic_store_explicit(&thread->release,true,memory_order_release);
#if defined(XR_OS_WINDOWS)
    CHECK(WaitForSingleObject(thread->handle,10000)==WAIT_OBJECT_0 && CloseHandle(thread->handle));
#else
    CHECK(pthread_join(thread->handle,NULL)==0);
#endif
}
static bool token_is_alive(int64_t token) {
    bool value=false;CHECK(xr_os_io_thread_token_alive(NULL,token,&value)==XR_OS_IO_OK);return value;
}
static void lifetime_controls(int64_t current) {
    TokenThread first,second,next;
    token_thread_start(&first,false);token_thread_start(&second,false);
    CHECK(first.status==XR_OS_IO_OK && second.status==XR_OS_IO_OK &&
        first.token>0 && first.token==first.again && second.token>0 && second.token==second.again &&
        first.token!=current && second.token!=current && first.token!=second.token);
    CHECK(token_is_alive(first.token) && token_is_alive(second.token) && token_is_alive(current));
    CHECK(xr_os_io_thread_token_shutdown()==XR_OS_IO_BAD_ARGUMENT);
    CHECK(token_is_alive(first.token) && token_is_alive(current));
    token_thread_join(&first);CHECK(!token_is_alive(first.token) && token_is_alive(second.token));
    token_thread_start(&next,false);
    CHECK(next.status==XR_OS_IO_OK && next.token==next.again && next.token!=first.token &&
        next.token!=second.token && next.token!=current && token_is_alive(next.token));
    token_thread_join(&second);token_thread_join(&next);
    CHECK(!token_is_alive(first.token) && !token_is_alive(second.token) && !token_is_alive(next.token));
}
static void failure_controls(int64_t current) {
    TokenThread failed;
    size_t live=process_live;
    atomic_store_explicit(&fail_allocate,true,memory_order_release);
    token_thread_start(&failed,false);
    CHECK(failed.status==XR_OS_IO_OUT_OF_MEMORY && failed.token==-17 && process_live==live);
    token_thread_join(&failed);
    _Atomic(bool) cancelled;atomic_init(&cancelled,false);
    XrOsThreadTokenQuery query={&cancelled,controlled_current};
    int64_t unchanged=-29;bool answer=true;size_t attempts=allocation_attempts;
    CHECK(xr_os_io_thread_token_current(&query,&unchanged)==XR_OS_IO_CANCELLED && unchanged==-29);
    CHECK(xr_os_io_thread_token_alive(&query,current,&answer)==XR_OS_IO_CANCELLED && answer &&
        attempts==allocation_attempts && process_live==live);
    /* This allocation really occurs on the new thread before its OS identity
     * is published. The synchronous request becomes cancelled in that span. */
    memset(&failed,0,sizeof(failed));
    cancel_allocate=&failed.current;
    token_thread_start(&failed,true);
    CHECK(failed.status==XR_OS_IO_CANCELLED && failed.token==-17 && process_live==live);
    token_thread_join(&failed);cancel_allocate=NULL;
#if defined(XR_OS_WINDOWS)
    TokenThread dead,blocked;token_thread_start(&dead,false);CHECK(dead.status==XR_OS_IO_OK);
    token_thread_join(&dead);attempts=allocation_attempts;
    unsigned faults=close_faults;atomic_store_explicit(&fail_close,true,memory_order_release);
    token_thread_start(&blocked,false);
    CHECK(blocked.status==XR_OS_IO_IO && blocked.token==-17 && close_faults==faults+1 &&
        allocation_attempts==attempts);
    token_thread_join(&blocked);
    CHECK(!token_is_alive(dead.token));
#endif
    CHECK(token_is_alive(current) && !token_is_alive(0) && !token_is_alive(-1) && !token_is_alive(INT64_MAX));
}
int main(void) {
    atomic_init(&fail_allocate,false);atomic_init(&fail_close,false);
    int64_t current=-1,again=-2;
    CHECK(xr_os_io_thread_token_current(NULL,&current)==XR_OS_IO_OK && current>0);
    CHECK(xr_os_io_thread_token_current(NULL,&again)==XR_OS_IO_OK && again==current);
    CHECK(xr_os_io_thread_token_current(NULL,NULL)==XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_thread_token_alive(NULL,current,NULL)==XR_OS_IO_BAD_ARGUMENT);
    XrOsThreadTokenQuery malformed={0};again=-2;
    CHECK(xr_os_io_thread_token_current(&malformed,&again)==XR_OS_IO_BAD_ARGUMENT && again==-2);
    lifetime_controls(current);failure_controls(current);
    unsigned faults=close_faults;atomic_store_explicit(&fail_close,true,memory_order_release);
    CHECK(xr_os_io_thread_token_shutdown()==XR_OS_IO_IO && close_faults==faults+1 && process_live);
    CHECK(xr_os_io_thread_token_shutdown()==XR_OS_IO_OK && !process_live);
    CHECK(xr_os_io_thread_token_shutdown()==XR_OS_IO_OK);
    again=-31;bool answer=true;
    CHECK(xr_os_io_thread_token_current(NULL,&again)==XR_OS_IO_BAD_ARGUMENT && again==-31);
    CHECK(xr_os_io_thread_token_alive(NULL,current,&answer)==XR_OS_IO_BAD_ARGUMENT && answer);
    puts("42");return 0;
}
