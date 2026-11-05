/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_owned_win.c - Stable file owners and isolated synchronous I/O workers
 */
#include "../os_file_owned.h"
#include "../file_metadata_path.inc.h"
#include "../../base/xwindows_utf8.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct OwnedFileState OwnedFileState;
typedef struct OwnedFileOp {
    XrOsFileOperation *anchor;
    struct OwnedFileOp *next;
    OwnedFileState *file;
    XrOsIoPolicy memory;
    XrOsFileAction action;
    HANDLE thread, duplicate, opened;
    FILE *stream;
    wchar_t *path;
    uint8_t *buffer;
    DWORD requested;
    bool append, terminal;
    _Atomic(bool) cancelled, complete;
    XrOsFileProgress result;
    XrOsIoStatus cancellation_error;
} OwnedFileOp;
struct OwnedFileState {
    XrOsFileOwner *anchor;
    XrOsIoPolicy memory;
    HANDLE handle;
    bool opening;
    OwnedFileOp *operations;
};
typedef struct OwnedFileConversion {
    const XrOsIoPolicy *memory;
    const XrOsFileQuery *query;
} OwnedFileConversion;

static XrOsIoStatus owned_file_error(DWORD error) {
    if(error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_OUTOFMEMORY)return XR_OS_IO_OUT_OF_MEMORY;
    if(error==ERROR_NOT_ENOUGH_QUOTA || error==ERROR_MAX_THRDS_REACHED)return XR_OS_IO_MEMORY_LIMIT;
    if(error==ERROR_ARITHMETIC_OVERFLOW)return XR_OS_IO_OVERFLOW;
    if(error==ERROR_OPERATION_ABORTED)return XR_OS_IO_CANCELLED;
    return XR_OS_IO_IO;
}
static bool owned_file_memory_valid(const XrOsIoPolicy *memory) {
    return memory && memory->context && memory->alloc && memory->free;
}
static bool owned_file_current(const XrOsFileQuery *query) {
    return !query || query->current(query->context);
}
static OwnedFileState *owned_file_state(XrOsFileOwner *owner) {
    OwnedFileState *state=owner?owner->state:NULL;
    return state && state->anchor==owner?state:NULL;
}
static OwnedFileOp *owned_file_operation(XrOsFileOperation *operation) {
    OwnedFileOp *op=operation?operation->state:NULL;
    return op && op->anchor==operation?op:NULL;
}
static XrOsIoStatus owned_file_allocate(const XrOsIoPolicy *memory,size_t size,size_t alignment,void **output) {
    void *block=NULL;XrOsIoStatus status=memory->alloc(memory->context,size,&block);
    if(status!=XR_OS_IO_OK) {if(block)memory->free(memory->context,block);return status;}
    if(!block)return XR_OS_IO_BAD_ARGUMENT;
    if((uintptr_t)block%alignment) {memory->free(memory->context,block);return XR_OS_IO_BAD_ARGUMENT;}
    memset(block,0,size);*output=block;return XR_OS_IO_OK;
}
static XrOsIoStatus owned_file_conversion_allocate(void *context,size_t size,void **output) {
    OwnedFileConversion *conversion=context;
    return conversion->memory->alloc(conversion->memory->context,size,output);
}
static void owned_file_conversion_free(void *context,void *block) {
    OwnedFileConversion *conversion=context;
    conversion->memory->free(conversion->memory->context,block);
}
static bool owned_file_conversion_current(void *context) {
    return owned_file_current(((OwnedFileConversion *)context)->query);
}
static XrOsIoStatus owned_file_path(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
    const XrOsFileRequest *request,wchar_t **output,bool *refused) {
    OwnedFileConversion conversion={memory,query};
    XrOsIoPolicy policy={&conversion,owned_file_conversion_allocate,owned_file_conversion_free};
    char *text=NULL;
    XrOsIoStatus status=metadata_path_owned(&policy,request->path,request->path_length,
        owned_file_conversion_current,&text);
    if(status==XR_OS_IO_OK && request->path_length>(size_t)32767*3)*refused=true;
    if(status==XR_OS_IO_OK && !*refused) {
        status=xr_win_utf8_path_owned_classified(&policy,text,output,refused);
        if(*refused)status=XR_OS_IO_OK;
    }
    if(text)memory->free(memory->context,text);
    if(status==XR_OS_IO_OK && !owned_file_current(query))status=XR_OS_IO_CANCELLED;
    return status;
}
static void owned_file_result_error(OwnedFileOp *op,DWORD error,bool opening) {
    op->result.native_error=error;
    if(error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_OUTOFMEMORY || error==ERROR_NOT_ENOUGH_QUOTA ||
        error==ERROR_MAX_THRDS_REACHED || error==ERROR_ARITHMETIC_OVERFLOW ||
        error==ERROR_OPERATION_ABORTED || error==ERROR_INVALID_HANDLE)op->result.status=owned_file_error(error);
    else if(opening && !xr_win_path_error_refused(error) && error!=ERROR_CANT_ACCESS_FILE)
        op->result.status=owned_file_error(error);
    else op->result.status=XR_OS_IO_OK;
}
static void owned_file_stream_io(OwnedFileOp *op) {
    errno=0;SetLastError(ERROR_SUCCESS);_set_doserrno(0);
    size_t count=0;bool okay=false;
    if(op->action==XR_OS_FILE_READ) {count=fread(op->buffer,1,op->requested,op->stream);okay=!ferror(op->stream);}
    else if(op->action==XR_OS_FILE_WRITE) {count=fwrite(op->buffer,1,op->requested,op->stream);okay=!ferror(op->stream);}
    else okay=fflush(op->stream)==0;
    op->result.count=count;
    if(okay) {op->result.success=true;return;}
    unsigned long error=0;
    if(_get_doserrno(&error)!=0 || !error)error=GetLastError();
    owned_file_result_error(op,(DWORD)error,false);
    if(op->result.status==XR_OS_IO_OK && count)op->result.success=true;
    /* A cancelled CRT read must not leave its error indicator poisoning the
     * next genuine read from the shared stream. Buffered bytes remain intact. */
    if(op->result.status==XR_OS_IO_CANCELLED)clearerr(op->stream);
}
static DWORD WINAPI owned_file_worker(void *context) {
    OwnedFileOp *op=context;
    if(atomic_load_explicit(&op->cancelled,memory_order_acquire))op->result.status=XR_OS_IO_CANCELLED;
    else if(op->action==XR_OS_FILE_OPEN_READ || op->action==XR_OS_FILE_OPEN_WRITE) {
        DWORD access=op->action==XR_OS_FILE_OPEN_READ?GENERIC_READ:(op->append?FILE_APPEND_DATA:GENERIC_WRITE);
        DWORD creation=op->action==XR_OS_FILE_OPEN_READ?OPEN_EXISTING:(op->append?OPEN_ALWAYS:CREATE_ALWAYS);
        HANDLE handle=CreateFileW(op->path,access,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,creation,FILE_ATTRIBUTE_NORMAL,NULL);
        if(handle==INVALID_HANDLE_VALUE)owned_file_result_error(op,GetLastError(),true);
        else {op->opened=handle;op->result.success=true;}
    } else if(op->stream)owned_file_stream_io(op);
    else {
        DWORD count=0;BOOL okay=op->action==XR_OS_FILE_READ?
            ReadFile(op->duplicate,op->buffer,op->requested,&count,NULL):
            WriteFile(op->duplicate,op->buffer,op->requested,&count,NULL);
        op->result.count=count;
        if(okay)op->result.success=true;
        else {
            DWORD error=GetLastError();
            if(op->action==XR_OS_FILE_READ && (error==ERROR_HANDLE_EOF || error==ERROR_BROKEN_PIPE))op->result.success=true;
            else {owned_file_result_error(op,error,false);
                if(op->result.status==XR_OS_IO_OK && count)op->result.success=true;}
        }
    }
    atomic_store_explicit(&op->complete,true,memory_order_release);return 0;
}
static XrOsIoStatus owned_file_request_cancel(OwnedFileOp *op) {
    atomic_store_explicit(&op->cancelled,true,memory_order_release);
    if(!op->thread || op->terminal)return XR_OS_IO_OK;
    DWORD wait=WaitForSingleObject(op->thread,0);
    if(wait==WAIT_OBJECT_0)return XR_OS_IO_OK;
    if(wait!=WAIT_TIMEOUT)return XR_OS_IO_IO;
    if(CancelSynchronousIo(op->thread))return XR_OS_IO_OK;
    DWORD error=GetLastError();return error==ERROR_NOT_FOUND?XR_OS_IO_OK:owned_file_error(error);
}
static XrOsIoStatus owned_file_probe(OwnedFileOp *op) {
    if(op->cancellation_error!=XR_OS_IO_OK) {
        XrOsIoStatus status=op->cancellation_error;op->cancellation_error=XR_OS_IO_OK;
        return status;
    }
    if(!op->terminal) {
        if(atomic_load_explicit(&op->cancelled,memory_order_acquire)) {
            XrOsIoStatus status=owned_file_request_cancel(op);if(status!=XR_OS_IO_OK)return status;
        }
        DWORD wait=WaitForSingleObject(op->thread,0);
        if(wait==WAIT_TIMEOUT)return XR_OS_IO_OK;
        if(wait!=WAIT_OBJECT_0 || !atomic_load_explicit(&op->complete,memory_order_acquire))return XR_OS_IO_IO;
        op->terminal=true;
    }
    /* Only the serialized caller publishes the worker-owned opening result,
     * after both thread termination and the complete release are observed. */
    if(op->file && op->file->opening &&
        (op->action==XR_OS_FILE_OPEN_READ || op->action==XR_OS_FILE_OPEN_WRITE)) {
        if(op->file->handle || (op->result.success && !op->opened))return XR_OS_IO_IO;
        op->file->handle=op->opened;op->opened=NULL;op->file->opening=false;
    }
    return XR_OS_IO_OK;
}
static XrOsIoStatus owned_file_op_new(const XrOsIoPolicy *memory,OwnedFileState *file,
    const XrOsFileRequest *request,XrOsFileOperation *operation,OwnedFileOp **output) {
    OwnedFileOp *op=NULL;
    XrOsIoStatus status=owned_file_allocate(memory,sizeof(*op),_Alignof(OwnedFileOp),(void **)&op);
    if(status!=XR_OS_IO_OK)return status;
    op->anchor=operation;op->file=file;op->memory=*memory;op->action=request->action;op->append=request->append;
    op->requested=request->length>MAXDWORD?MAXDWORD:(DWORD)request->length;
    atomic_init(&op->cancelled,false);atomic_init(&op->complete,false);
    if(file) {op->next=file->operations;file->operations=op;}
    operation->state=op;*output=op;return XR_OS_IO_OK;
}
static XrOsIoStatus owned_file_launch(OwnedFileOp *op,const XrOsFileQuery *query,XrOsIoStatus status) {
    if(status==XR_OS_IO_OK && !owned_file_current(query))status=XR_OS_IO_CANCELLED;
    if(status==XR_OS_IO_OK && !(op->thread=CreateThread(NULL,0,owned_file_worker,op,0,NULL)))
        status=owned_file_error(GetLastError());
    if(!op->thread) {
        op->terminal=true;op->result.status=status;
        atomic_store_explicit(&op->complete,true,memory_order_release);
    } else if(!owned_file_current(query)) {
        status=XR_OS_IO_CANCELLED;
        /* The begin result keeps its first cancellation. Poll/close reports
         * a failed cancellation request once before another close-only try. */
        op->cancellation_error=owned_file_request_cancel(op);
    }
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_file_open_begin(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
    const XrOsFileRequest *request,XrOsFileOwner *owner,XrOsFileOperation *operation) {
    if(!owned_file_memory_valid(memory) || (query && !query->current) || !request ||
        (request->action!=XR_OS_FILE_OPEN_READ && request->action!=XR_OS_FILE_OPEN_WRITE) ||
        request->standard_stream!=-1 || !request->path || !owner || owner->state || owner->busy ||
        !operation || operation->state || operation->busy)return XR_OS_IO_BAD_ARGUMENT;
    if(!owned_file_current(query))return XR_OS_IO_CANCELLED;
    owner->busy=operation->busy=true;OwnedFileState *file=NULL;OwnedFileOp *op=NULL;
    XrOsIoStatus status=owned_file_allocate(memory,sizeof(*file),_Alignof(OwnedFileState),(void **)&file);
    if(status==XR_OS_IO_OK) {file->anchor=owner;file->memory=*memory;file->opening=true;owner->state=file;
        status=owned_file_op_new(memory,file,request,operation,&op);}
    if(status==XR_OS_IO_OK) {
        bool refused=false;status=owned_file_path(memory,query,request,&op->path,&refused);
        if(status==XR_OS_IO_OK && refused) {op->terminal=true;atomic_store_explicit(&op->complete,true,memory_order_release);}
        else status=owned_file_launch(op,query,status);
    }
    owner->busy=operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_file_begin(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
    const XrOsFileRequest *request,XrOsFileOwner *owner,XrOsFileOperation *operation) {
    OwnedFileState *file=owned_file_state(owner);
    if(!owned_file_memory_valid(memory) || (query && !query->current) || !request ||
        request->action<XR_OS_FILE_READ || request->action>XR_OS_FILE_FLUSH ||
        !operation || operation->state || operation->busy ||
        (owner?(!file || owner->busy || file->opening || !file->handle || request->standard_stream!=-1):
            (request->standard_stream<0 || request->standard_stream>2)) ||
        (request->action==XR_OS_FILE_WRITE?(!request->bytes && request->length):request->bytes!=NULL))return XR_OS_IO_BAD_ARGUMENT;
    if(!owned_file_current(query))return XR_OS_IO_CANCELLED;
    if(owner)owner->busy=true;operation->busy=true;OwnedFileOp *op=NULL;
    XrOsIoStatus status=owned_file_op_new(memory,file,request,operation,&op);
    if(status==XR_OS_IO_OK) {
        if(!owner)op->stream=request->standard_stream==0?stdin:(request->standard_stream==1?stdout:stderr);
        if(request->action!=XR_OS_FILE_FLUSH && op->requested)
            status=owned_file_allocate(memory,op->requested,_Alignof(uint8_t),(void **)&op->buffer);
        if(status==XR_OS_IO_OK && request->action==XR_OS_FILE_WRITE) {
            size_t offset=0;
            while(offset<op->requested) {
                if(!owned_file_current(query)) {status=XR_OS_IO_CANCELLED;break;}
                size_t count=op->requested-offset;if(count>65536)count=65536;
                memcpy(op->buffer+offset,request->bytes+offset,count);offset+=count;
            }
        }
        bool immediate=(request->action!=XR_OS_FILE_FLUSH && !op->requested) ||
            (request->action==XR_OS_FILE_FLUSH && owner);
        if(status==XR_OS_IO_OK && !owned_file_current(query))status=XR_OS_IO_CANCELLED;
        if(status==XR_OS_IO_OK && immediate) {
            op->terminal=true;op->result.success=true;atomic_store_explicit(&op->complete,true,memory_order_release);
        } else {
            if(status==XR_OS_IO_OK && owner && !DuplicateHandle(GetCurrentProcess(),file->handle,
                GetCurrentProcess(),&op->duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))status=owned_file_error(GetLastError());
            status=owned_file_launch(op,query,status);
        }
    }
    if(owner)owner->busy=false;operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_file_poll(XrOsFileOperation *operation,XrOsFileProgress *output) {
    OwnedFileOp *op=owned_file_operation(operation);
    if(!op || operation->busy || !output)return XR_OS_IO_BAD_ARGUMENT;
    operation->busy=true;XrOsIoStatus status=owned_file_probe(op);
    if(status==XR_OS_IO_OK) {
        XrOsFileProgress progress={0};progress.terminal=op->terminal;
        if(op->terminal) {progress=op->result;progress.terminal=true;
            if(op->action==XR_OS_FILE_READ && progress.success)progress.bytes=op->buffer;}
        *output=progress;
    }
    operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_file_cancel(XrOsFileOperation *operation) {
    OwnedFileOp *op=owned_file_operation(operation);if(!op || operation->busy)return XR_OS_IO_BAD_ARGUMENT;
    operation->busy=true;XrOsIoStatus status=owned_file_request_cancel(op);operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_file_operation_close(XrOsFileOperation *operation,bool *closed) {
    if(!operation || operation->busy || !closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!operation->state) {*closed=true;return XR_OS_IO_OK;}
    OwnedFileOp *op=owned_file_operation(operation);
    if(!op || (op->file && op->file->anchor->busy))return XR_OS_IO_BAD_ARGUMENT;
    operation->busy=true;if(op->file)op->file->anchor->busy=true;
    XrOsIoStatus status=owned_file_probe(op);
    if(status==XR_OS_IO_OK && !op->terminal) {status=owned_file_request_cancel(op);if(status==XR_OS_IO_OK)*closed=false;}
    else if(status==XR_OS_IO_OK) {
        if(op->thread) {if(CloseHandle(op->thread))op->thread=NULL;else status=owned_file_error(GetLastError());}
        if(status==XR_OS_IO_OK && op->duplicate) {
            if(CloseHandle(op->duplicate))op->duplicate=NULL;else status=owned_file_error(GetLastError());
        }
        if(status==XR_OS_IO_OK) {
            if(op->file) {
                OwnedFileOp **link=&op->file->operations;while(*link && *link!=op)link=&(*link)->next;
                if(*link!=op)status=XR_OS_IO_BAD_ARGUMENT;
                else {*link=op->next;op->file->anchor->busy=false;}
            }
            if(status==XR_OS_IO_OK) {
                XrOsIoPolicy memory=op->memory;operation->state=NULL;
                if(op->path)memory.free(memory.context,op->path);
                if(op->buffer)memory.free(memory.context,op->buffer);
                memory.free(memory.context,op);*closed=true;operation->busy=false;return XR_OS_IO_OK;
            }
        }
    }
    if(op->file)op->file->anchor->busy=false;operation->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_file_close(XrOsFileOwner *owner,bool *closed) {
    if(!owner || owner->busy || !closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!owner->state) {*closed=true;return XR_OS_IO_OK;}
    OwnedFileState *file=owned_file_state(owner);if(!file)return XR_OS_IO_BAD_ARGUMENT;
    if(file->operations) {*closed=false;return XR_OS_IO_OK;}
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(file->handle) {if(CloseHandle(file->handle))file->handle=NULL;else status=owned_file_error(GetLastError());}
    if(status==XR_OS_IO_OK) {
        XrOsIoPolicy memory=file->memory;owner->state=NULL;memory.free(memory.context,file);*closed=true;
    }
    owner->busy=false;return status;
}
