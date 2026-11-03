/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_owned_win.c - One real synchronous pipe syscall per owned OS worker
 */
#include "../os_pipe_owned.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct OwnedPipeState OwnedPipeState;
typedef struct OwnedPipeOp {
    XrOsPipeOperation *anchor;
    struct OwnedPipeOp *next;
    OwnedPipeState *pipe;
    HANDLE thread,duplicate;
    uint8_t *buffer;
    DWORD requested;
    XrOsPipeEnd end;
    _Atomic(bool) cancelled,complete;
    bool terminal;
    XrOsPipeProgress result;
} OwnedPipeOp;
struct OwnedPipeState {
    XrOsPipeOwner *anchor;
    XrOsIoPolicy memory;
    HANDLE ends[2];
    OwnedPipeOp *operations;
};

static XrOsIoStatus owned_pipe_error(DWORD error){
    if(error==ERROR_NOT_ENOUGH_MEMORY||error==ERROR_OUTOFMEMORY)return XR_OS_IO_OUT_OF_MEMORY;
    if(error==ERROR_NOT_ENOUGH_QUOTA||error==ERROR_MAX_THRDS_REACHED)return XR_OS_IO_MEMORY_LIMIT;
    if(error==ERROR_ARITHMETIC_OVERFLOW)return XR_OS_IO_OVERFLOW;
    return XR_OS_IO_IO;
}
static bool owned_pipe_query_valid(const XrOsPipeQuery *query){return !query||query->current;}
static bool owned_pipe_current(const XrOsPipeQuery *query){return !query||query->current(query->context);}
static OwnedPipeState *owned_pipe_state(XrOsPipeOwner *owner){
    OwnedPipeState *state=owner?owner->state:NULL;return state&&state->anchor==owner?state:NULL;
}
static OwnedPipeOp *owned_pipe_op(XrOsPipeOperation *owner){
    OwnedPipeOp *state=owner?owner->state:NULL;return state&&state->anchor==owner?state:NULL;
}
static XrOsIoStatus owned_pipe_allocate(const XrOsIoPolicy *memory,size_t bytes,void **output){
    void *allocated=NULL;XrOsIoStatus status=memory->alloc(memory->context,bytes,&allocated);
    if(status!=XR_OS_IO_OK){if(allocated)memory->free(memory->context,allocated);return status;}
    if(!allocated)return XR_OS_IO_BAD_ARGUMENT;
    memset(allocated,0,bytes);*output=allocated;return XR_OS_IO_OK;
}
static DWORD WINAPI owned_pipe_worker(void *context){
    OwnedPipeOp *op=context;DWORD count=0,error=ERROR_SUCCESS;BOOL okay=FALSE;
    if(atomic_load_explicit(&op->cancelled,memory_order_acquire)){
        op->result.status=XR_OS_IO_CANCELLED;
    }else{
        okay=op->end==XR_OS_PIPE_READ?ReadFile(op->duplicate,op->buffer,op->requested,&count,NULL):
            WriteFile(op->duplicate,op->buffer,op->requested,&count,NULL);
        if(!okay)error=GetLastError();
        op->result.native_error=error;
        if(okay){
            op->result.status=count<=op->requested?XR_OS_IO_OK:XR_OS_IO_IO;
            op->result.count=count<=op->requested?count:0;
        }else if(error==ERROR_OPERATION_ABORTED)op->result.status=XR_OS_IO_CANCELLED;
        else if(op->end==XR_OS_PIPE_READ&&(error==ERROR_BROKEN_PIPE||error==ERROR_HANDLE_EOF))op->result.status=XR_OS_IO_OK;
        else if(op->end==XR_OS_PIPE_WRITE&&(error==ERROR_BROKEN_PIPE||error==ERROR_NO_DATA||error==ERROR_PIPE_NOT_CONNECTED))op->result.status=XR_OS_IO_END;
        else op->result.status=owned_pipe_error(error);
    }
    atomic_store_explicit(&op->complete,true,memory_order_release);return 0;
}
static XrOsIoStatus owned_pipe_request_cancel(OwnedPipeOp *op){
    atomic_store_explicit(&op->cancelled,true,memory_order_release);
    if(!op->thread||op->terminal)return XR_OS_IO_OK;
    DWORD wait=WaitForSingleObject(op->thread,0);
    if(wait==WAIT_OBJECT_0)return XR_OS_IO_OK;
    if(wait!=WAIT_TIMEOUT)return XR_OS_IO_IO;
    if(CancelSynchronousIo(op->thread))return XR_OS_IO_OK;
    DWORD error=GetLastError();return error==ERROR_NOT_FOUND?XR_OS_IO_OK:owned_pipe_error(error);
}
static XrOsIoStatus owned_pipe_probe(OwnedPipeOp *op){
    if(op->terminal)return XR_OS_IO_OK;
    if(atomic_load_explicit(&op->cancelled,memory_order_acquire)){
        XrOsIoStatus status=owned_pipe_request_cancel(op);if(status!=XR_OS_IO_OK)return status;
    }
    DWORD wait=WaitForSingleObject(op->thread,0);
    if(wait==WAIT_TIMEOUT)return XR_OS_IO_OK;
    if(wait!=WAIT_OBJECT_0||!atomic_load_explicit(&op->complete,memory_order_acquire))return XR_OS_IO_IO;
    op->terminal=true;return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_open(const XrOsIoPolicy *memory,const XrOsPipeQuery *query,XrOsPipeOwner *owner,bool *opened){
    if(!memory||!memory->context||!memory->alloc||!memory->free||!owner||owner->state||owner->busy||!opened||!owned_pipe_query_valid(query))return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(!owned_pipe_current(query)){owner->busy=false;return XR_OS_IO_CANCELLED;}
    OwnedPipeState *state=NULL;status=owned_pipe_allocate(memory,sizeof(*state),(void **)&state);
    if(status==XR_OS_IO_OK){
        state->anchor=owner;state->memory=*memory;owner->state=state;
        if(!owned_pipe_current(query))status=XR_OS_IO_CANCELLED;
        else if(!CreatePipe(&state->ends[0],&state->ends[1],NULL,0)){
            DWORD error=GetLastError();
            if(error==ERROR_ACCESS_DENIED){*opened=false;status=XR_OS_IO_OK;}
            else status=owned_pipe_error(error);
        }else if(!owned_pipe_current(query))status=XR_OS_IO_CANCELLED;
        else *opened=true;
    }
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_begin(XrOsPipeOwner *owner,const XrOsPipeRequest *request,const XrOsPipeQuery *query,XrOsPipeOperation *operation){
    OwnedPipeState *pipe=owned_pipe_state(owner);
    if(!pipe||owner->busy||!operation||operation->state||operation->busy||!request||(request->end!=XR_OS_PIPE_READ&&request->end!=XR_OS_PIPE_WRITE)||
        (request->end==XR_OS_PIPE_READ?request->bytes!=NULL:(!request->bytes&&request->length))||!owned_pipe_query_valid(query))return XR_OS_IO_BAD_ARGUMENT;
    if(request->length>(uint64_t)INT64_MAX)return XR_OS_IO_OVERFLOW;
    if(!pipe->ends[request->end])return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=operation->busy=true;XrOsIoStatus status=XR_OS_IO_OK;OwnedPipeOp *op=NULL;
    if(!owned_pipe_current(query))status=XR_OS_IO_CANCELLED;
    if(status==XR_OS_IO_OK)status=owned_pipe_allocate(&pipe->memory,sizeof(*op),(void **)&op);
    if(status==XR_OS_IO_OK){
        op->anchor=operation;op->pipe=pipe;op->next=pipe->operations;pipe->operations=op;operation->state=op;
        op->end=request->end;op->requested=request->length>MAXDWORD?MAXDWORD:(DWORD)request->length;
        atomic_init(&op->cancelled,false);atomic_init(&op->complete,false);
        if(op->requested)status=owned_pipe_allocate(&pipe->memory,op->requested,(void **)&op->buffer);
        if(status==XR_OS_IO_OK&&op->end==XR_OS_PIPE_WRITE){
            size_t at=0;
            while(at<op->requested){
                if(!owned_pipe_current(query)){status=XR_OS_IO_CANCELLED;break;}
                size_t count=op->requested-at;if(count>65536)count=65536;
                memcpy(op->buffer+at,request->bytes+at,count);at+=count;
            }
        }
        if(status==XR_OS_IO_OK&&!owned_pipe_current(query))status=XR_OS_IO_CANCELLED;
        if(status==XR_OS_IO_OK&&op->requested){
            if(!DuplicateHandle(GetCurrentProcess(),pipe->ends[request->end],GetCurrentProcess(),&op->duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))status=owned_pipe_error(GetLastError());
            else if(!owned_pipe_current(query))status=XR_OS_IO_CANCELLED;
            else if(!(op->thread=CreateThread(NULL,0,owned_pipe_worker,op,0,NULL)))status=owned_pipe_error(GetLastError());
        }
        if(!op->thread){op->terminal=true;op->result.status=status;atomic_store_explicit(&op->complete,true,memory_order_release);}
        else if(!owned_pipe_current(query)){status=XR_OS_IO_CANCELLED;XrOsIoStatus cancelled=owned_pipe_request_cancel(op);if(cancelled!=XR_OS_IO_OK)status=cancelled;}
    }
    owner->busy=operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_poll(XrOsPipeOperation *operation,XrOsPipeProgress *output){
    OwnedPipeOp *op=owned_pipe_op(operation);if(!op||operation->busy||!output)return XR_OS_IO_BAD_ARGUMENT;
    operation->busy=true;XrOsIoStatus status=owned_pipe_probe(op);
    if(status==XR_OS_IO_OK){
        XrOsPipeProgress progress={0};progress.terminal=op->terminal;
        if(op->terminal){progress=op->result;progress.terminal=true;if(op->end==XR_OS_PIPE_READ&&progress.status==XR_OS_IO_OK)progress.bytes=op->buffer;}
        *output=progress;
    }
    operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_cancel(XrOsPipeOperation *operation){
    OwnedPipeOp *op=owned_pipe_op(operation);if(!op||operation->busy)return XR_OS_IO_BAD_ARGUMENT;
    operation->busy=true;XrOsIoStatus status=owned_pipe_request_cancel(op);operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_operation_close(XrOsPipeOperation *operation,bool *closed){
    if(!operation||operation->busy||!closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!operation->state){*closed=true;return XR_OS_IO_OK;}
    OwnedPipeOp *op=owned_pipe_op(operation);if(!op||op->pipe->anchor->busy)return XR_OS_IO_BAD_ARGUMENT;
    XrOsPipeOwner *pair=op->pipe->anchor;
    pair->busy=operation->busy=true;XrOsIoStatus status=owned_pipe_probe(op);
    if(status==XR_OS_IO_OK&&!op->terminal){status=owned_pipe_request_cancel(op);if(status==XR_OS_IO_OK)*closed=false;}
    else if(status==XR_OS_IO_OK){
        if(op->thread){if(CloseHandle(op->thread))op->thread=NULL;else status=owned_pipe_error(GetLastError());}
        if(status==XR_OS_IO_OK&&op->duplicate){if(CloseHandle(op->duplicate))op->duplicate=NULL;else status=owned_pipe_error(GetLastError());}
        if(status==XR_OS_IO_OK){
            OwnedPipeState *pipe=op->pipe;OwnedPipeOp **link=&pipe->operations;
            while(*link&&*link!=op)link=&(*link)->next;
            if(!*link)status=XR_OS_IO_BAD_ARGUMENT;
            else{*link=op->next;operation->state=NULL;if(op->buffer)pipe->memory.free(pipe->memory.context,op->buffer);pipe->memory.free(pipe->memory.context,op);*closed=true;}
        }
    }
    pair->busy=operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_close_end(XrOsPipeOwner *owner,XrOsPipeEnd end,bool *closed){
    if(!owner||owner->busy||(end!=XR_OS_PIPE_READ&&end!=XR_OS_PIPE_WRITE)||!closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!owner->state){*closed=true;return XR_OS_IO_OK;}
    OwnedPipeState *pipe=owned_pipe_state(owner);if(!pipe)return XR_OS_IO_BAD_ARGUMENT;
    for(OwnedPipeOp *op=pipe->operations;op;op=op->next)if(op->end==end){*closed=false;return XR_OS_IO_OK;}
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(pipe->ends[end]){if(CloseHandle(pipe->ends[end]))pipe->ends[end]=NULL;else status=owned_pipe_error(GetLastError());}
    if(status==XR_OS_IO_OK)*closed=true;owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_dispose(XrOsPipeOwner *owner,bool *closed){
    if(!owner||owner->busy||!closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!owner->state){*closed=true;return XR_OS_IO_OK;}
    OwnedPipeState *pipe=owned_pipe_state(owner);if(!pipe)return XR_OS_IO_BAD_ARGUMENT;
    if(pipe->operations){*closed=false;return XR_OS_IO_OK;}
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    for(unsigned end=0;end<2&&status==XR_OS_IO_OK;++end)if(pipe->ends[end]){
        if(CloseHandle(pipe->ends[end]))pipe->ends[end]=NULL;else status=owned_pipe_error(GetLastError());
    }
    if(status==XR_OS_IO_OK){XrOsIoPolicy memory=pipe->memory;owner->state=NULL;memory.free(memory.context,pipe);*closed=true;}
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_pipe_borrow(XrOsPipeOwner *owner,XrOsPipeEnd end,intptr_t *output){
    OwnedPipeState *pipe=owned_pipe_state(owner);
    if(!pipe||owner->busy||(end!=XR_OS_PIPE_READ&&end!=XR_OS_PIPE_WRITE)||!output||!pipe->ends[end])return XR_OS_IO_BAD_ARGUMENT;
    *output=(intptr_t)pipe->ends[end];return XR_OS_IO_OK;
}
