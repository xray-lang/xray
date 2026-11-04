/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_complete_os_runtime.h - Actual shell capture beyond both pipe buffers
 *
 * KEY CONCEPT:
 *   Activation termination retires capture operations. Root retirement retains
 *   the separate process/job duty; it is never called activation cleanup.
 */
#ifndef PROCESS_COMPLETE_OS_RUNTIME_H
#define PROCESS_COMPLETE_OS_RUNTIME_H
enum {PROCESS_COMPLETE_OS_ENTRY,PROCESS_COMPLETE_OS_EXEC,PROCESS_COMPLETE_OS_FIELDS,
    PROCESS_COMPLETE_OS_OPEN,PROCESS_COMPLETE_OS_CLOSE,PROCESS_COMPLETE_OS_ORDINARY_CLOSE,PROCESS_COMPLETE_OS_COUNT};
#if defined(XR_OS_WINDOWS)
typedef struct ProcessCompleteOsRecord {DWORD pid,completed;uint64_t output_length,error_length;} ProcessCompleteOsRecord;
typedef struct ProcessCompleteOsChild {
    HANDLE mapping,ready,release,done,process;
    ProcessCompleteOsRecord *record;
    char name[128];bool allow_output,spaced_argument;
} ProcessCompleteOsChild;
static unsigned process_complete_os_serial;
static bool process_complete_os_observing,process_complete_os_oom,process_complete_os_pending;
static unsigned process_complete_os_acquired,process_complete_os_retired,process_complete_os_hits;
static unsigned process_complete_os_closed_ends[2];
static unsigned process_complete_os_close_attempts[2],process_complete_os_disposals,process_complete_os_close_faults;
static bool process_complete_os_fail_close;
static size_t process_complete_os_allocation;
static XrOsIoStatus process_complete_os_begin(XrOsPipeOwner *owner,const XrOsPipeRequest *request,
    const XrOsPipeQuery *query,XrOsPipeOperation *operation){
    bool owned=operation&&operation->state!=NULL;
    XrOsIoStatus status=xr_os_io_pipe_begin(owner,request,query,operation);
    if(process_complete_os_observing&&!owned&&operation&&operation->state)++process_complete_os_acquired;
    return status;
}
static XrOsIoStatus process_complete_os_poll(XrOsPipeOperation *operation,XrOsPipeProgress *output){
    XrOsIoStatus status=xr_os_io_pipe_poll(operation,output);
    if(process_complete_os_observing&&status==XR_OS_IO_OK&&output){
        if(!output->terminal)process_complete_os_pending=true;
        if(process_complete_os_oom&&output->terminal&&output->status==XR_OS_IO_OK&&output->count&&output->bytes){
            process_complete_os_oom=false;++process_complete_os_hits;
            process_complete_os_allocation=runtime_attempts;runtime_fail_at=runtime_attempts;
        }
    }
    return status;
}
static XrOsIoStatus process_complete_os_operation_close(XrOsPipeOperation *operation,bool *closed){
    bool owned=operation&&operation->state!=NULL;
    XrOsIoStatus status=xr_os_io_pipe_operation_close(operation,closed);
    if(process_complete_os_observing&&owned&&status==XR_OS_IO_OK&&closed&&*closed)++process_complete_os_retired;
    return status;
}
static XrOsIoStatus process_complete_os_close_end(XrOsPipeOwner *owner,XrOsPipeEnd end,bool *closed){
    if(process_complete_os_observing){
        CHECK(end==XR_OS_PIPE_READ||end==XR_OS_PIPE_WRITE);++process_complete_os_close_attempts[end==XR_OS_PIPE_WRITE];
        if(process_complete_os_fail_close&&end==XR_OS_PIPE_READ){
            /* Inject a typed provider failure without consuming this endpoint. */
            process_complete_os_fail_close=false;++process_complete_os_close_faults;
            return XR_OS_IO_IO;
        }
    }
    XrOsIoStatus status=xr_os_io_pipe_close_end(owner,end,closed);
    if(process_complete_os_observing&&status==XR_OS_IO_OK&&closed&&*closed){
        CHECK(end==XR_OS_PIPE_READ||end==XR_OS_PIPE_WRITE);++process_complete_os_closed_ends[end==XR_OS_PIPE_WRITE];
    }
    return status;
}
static XrOsIoStatus process_complete_os_dispose(XrOsPipeOwner *owner,bool *closed){
    bool owned=owner&&owner->state!=NULL;XrOsIoStatus status=xr_os_io_pipe_dispose(owner,closed);
    if(process_complete_os_observing&&owned&&status==XR_OS_IO_OK&&closed&&*closed)++process_complete_os_disposals;
    return status;
}
static void process_complete_os_open(ProcessCompleteOsChild *child,bool output){
    memset(child,0,sizeof(*child));child->allow_output=output;
    int count=snprintf(child->name,sizeof(child->name),"Local\\xray-complete-os-%lu-%u",(unsigned long)GetCurrentProcessId(),++process_complete_os_serial);
    CHECK(count>0&&(size_t)count<sizeof(child->name));char name[160];process_object_name(child->name,"map",name);
    child->mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)sizeof(*child->record),name);
    CHECK(child->mapping&&GetLastError()!=ERROR_ALREADY_EXISTS);
    child->record=MapViewOfFile(child->mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*child->record));CHECK(child->record);memset(child->record,0,sizeof(*child->record));
    child->ready=process_event(child->name,"ready",true);child->release=process_event(child->name,"release",true);child->done=process_event(child->name,"done",true);
}
static void process_complete_os_observe(ProcessCompleteOsChild *child){
    if(!child->process&&WaitForSingleObject(child->ready,0)==WAIT_OBJECT_0){
        CHECK(child->record->pid);child->process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,child->record->pid);CHECK(child->process);
        if(child->allow_output)CHECK(SetEvent(child->release));
    }
}
static void process_complete_os_close(ProcessCompleteOsChild *child){
    if(child->process)CHECK(CloseHandle(child->process));
    CHECK(CloseHandle(child->ready)&&CloseHandle(child->release)&&CloseHandle(child->done)&&UnmapViewOfFile(child->record)&&CloseHandle(child->mapping));
}
static uint64_t process_complete_os_size(HANDLE pipe,uint64_t extra){
    DWORD output=0,input=0;CHECK(GetNamedPipeInfo(pipe,NULL,&output,&input,NULL));
    uint64_t capacity=output>input?output:input;CHECK(capacity&&capacity<=UINT64_MAX-extra);return capacity+extra;
}
static void process_complete_os_write(HANDLE pipe,uint64_t length,unsigned seed,unsigned increment){
    unsigned char buffer[4096];unsigned value=seed;uint64_t offset=0;
    while(offset<length){
        size_t count=length-offset>sizeof(buffer)?sizeof(buffer):(size_t)(length-offset);
        for(size_t n=0;n<count;++n){buffer[n]=(unsigned char)value;value=(value+increment)&255u;}
        size_t sent=0;while(sent<count){DWORD written=0;CHECK(WriteFile(pipe,buffer+sent,(DWORD)(count-sent),&written,NULL)&&written&&written<=count-sent);sent+=written;}
        offset+=count;
    }
}
static bool process_complete_os_child(int argc,char **argv){
    if(argc<2||strcmp(argv[1],"--process-exec-child"))return false;
    CHECK(argc>=3);bool spaced=!strcmp(argv[2],"flood-spaced");CHECK(argc==(spaced?5:4));
    if(spaced)CHECK(!strcmp(argv[4],"argument with spaces"));char name[160];process_object_name(argv[3],"map",name);
    HANDLE mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name);CHECK(mapping);
    ProcessCompleteOsRecord *record=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*record));CHECK(record);
    HANDLE ready=process_event(argv[3],"ready",false),release=process_event(argv[3],"release",false),done=process_event(argv[3],"done",false);
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE),error=GetStdHandle(STD_ERROR_HANDLE);
    record->pid=GetCurrentProcessId();record->output_length=process_complete_os_size(output,65537);record->error_length=process_complete_os_size(error,65793);
    CHECK(SetEvent(ready)&&WaitForSingleObject(release,30000)==WAIT_OBJECT_0);
    CHECK(!strcmp(argv[2],"flood")||!strcmp(argv[2],"hold")||spaced);
    process_complete_os_write(output,record->output_length,11,37);process_complete_os_write(error,record->error_length,197,53);
    record->completed=1;CHECK(SetEvent(done));
    CHECK(CloseHandle(ready)&&CloseHandle(release)&&CloseHandle(done)&&UnmapViewOfFile(record)&&CloseHandle(mapping));ExitProcess(23);
}
static char *process_complete_os_command(ProcessCompleteOsChild *child){
    wchar_t executable[32768];DWORD length=GetModuleFileNameW(NULL,executable,32768);CHECK(length&&length<32768);char *utf8=process_utf8(executable);
    size_t bytes=strlen(utf8);CHECK(bytes<=SIZE_MAX-sizeof(child->name)-96);size_t capacity=bytes+sizeof(child->name)+96;char *command=xr_malloc(capacity);CHECK(command);
    const char *mode=child->spaced_argument?"flood-spaced":child->allow_output?"flood":"hold";
    int count=snprintf(command,capacity,"\"%s\" --process-exec-child %s %s%s",utf8,mode,child->name,
        child->spaced_argument?" \"argument with spaces\"":"");
    CHECK(count>0&&(size_t)count<capacity);xr_free(utf8);return command;
}
static XrXirInstanceResult process_complete_os_step(XrXirInstance *instance,ProcessCompleteOsChild *child){
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);process_complete_os_observe(child);
    if(event.outcome.status==XR_XIR_CALL_SUSPENDED){
        XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
        if(request.kind==XR_XIR_WAIT_TASK)xr_thread_yield();else process_resume(instance,event);
    }
    return event;
}
static XrXirCallStatus process_complete_os_drive(XrXirInstance *instance,ProcessCompleteOsChild *child){
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;){
        XrXirCallStatus status=process_complete_os_step(instance,child).outcome.status;
        if(status!=XR_XIR_CALL_READY&&status!=XR_XIR_CALL_SUSPENDED)return status;
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(90000000000));
    }
}
static void process_complete_os_arm(bool oom){
    CHECK(!process_oom_observing&&!process_oom_armed&&!process_complete_os_observing&&runtime_fail_at==SIZE_MAX);
    process_oom_prepares=process_oom_starts=0;process_oom_observing=true;process_complete_os_observing=true;process_complete_os_oom=oom;
    process_complete_os_pending=false;process_complete_os_acquired=process_complete_os_retired=process_complete_os_hits=0;
    process_complete_os_closed_ends[0]=process_complete_os_closed_ends[1]=0;
    process_complete_os_close_attempts[0]=process_complete_os_close_attempts[1]=0;
    process_complete_os_disposals=process_complete_os_close_faults=0;process_complete_os_fail_close=false;
}
static void process_complete_os_disarm(void){
    process_oom_observing=process_complete_os_observing=process_complete_os_oom=false;
}
static void process_complete_os_bytes(const XrXirValue *array,uint64_t total,int64_t limit,bool error){
    XrXirValueAdmission access=process_access(array);int64_t length=-1;
    uint64_t expected=total<(uint64_t)limit?total:(uint64_t)limit;CHECK(expected<=INT64_MAX);
    CHECK(xr_xir_array_len(array,&access,&length)==XR_XIR_VALUE_OK&&length==(int64_t)expected);
    for(int64_t n=0;n<length;++n){
        unsigned value=error?(197u+53u*(unsigned)(n&255))&255u:(11u+37u*(unsigned)(n&255))&255u;
        XrXirValue byte={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(array,n,&access,&byte,&fault)==XR_XIR_VALUE_OK&&byte.type==XR_XIR_U8&&byte.payload==(int64_t)value);xr_xir_value_drop(&byte);
    }
}
static void process_complete_os_result(XrXirInstance *instance,const uint32_t *ids,const XrXirValue *result,
    const ProcessCompleteOsChild *child,const int64_t limits[2]){
    const XrXirValue *object=process_some(result,true);XrXirValue fields=process_invoke(instance,ids[PROCESS_COMPLETE_OS_FIELDS],object,1),values[5]={{0}};
    for(unsigned n=0;n<5;++n)CHECK(xr_xir_tuple_get(&fields,n,&values[n])==XR_XIR_VALUE_OK);
    process_complete_os_bytes(&values[0],child->record->output_length,limits[0],false);process_complete_os_bytes(&values[1],child->record->error_length,limits[1],true);
    CHECK(values[2].type==XR_XIR_I64&&values[2].payload==23);
    CHECK(values[3].type==XR_XIR_BOOL&&values[3].payload==(child->record->output_length>(uint64_t)limits[0]));
    CHECK(values[4].type==XR_XIR_BOOL&&values[4].payload==(child->record->error_length>(uint64_t)limits[1]));
    for(unsigned n=0;n<5;++n)xr_xir_value_drop(&values[n]);xr_xir_value_drop(&fields);
}
static void process_complete_os_diagnostic_bytes(XrXirInstance *instance,const XrXirValue *array,const char *name){
    XrXirValueAdmission access={.arena=xr_xir_value_arena(array),.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};int64_t length=-1;
    XrXirValueStatus status=xr_xir_array_len(array,&access,&length);
    fprintf(stderr,"complete os.exec %s type%u lenStatus%u length%lld hex:",name,array->type,(unsigned)status,(long long)length);
    if(status==XR_XIR_VALUE_OK)for(int64_t n=0;n<length&&n<256;++n){
        XrXirValue byte={0};XrXirFaultDetail fault={0};status=xr_xir_array_get(array,n,&access,&byte,&fault);
        if(status==XR_XIR_VALUE_OK&&byte.type==XR_XIR_U8)fprintf(stderr," %02x",(unsigned)byte.payload);
        else fprintf(stderr," [status%u type%u]",(unsigned)status,byte.type);
        xr_xir_value_drop(&byte);if(status!=XR_XIR_VALUE_OK)break;
    }
    fputc('\n',stderr);
}
static void process_complete_os_diagnostic(XrXirInstance *instance,const uint32_t *ids,const XrXirValue *result,
    const ProcessCompleteOsChild *child,const char *command){
    DWORD exit_code=0;BOOL queried=child->process?GetExitCodeProcess(child->process,&exit_code):FALSE;
    bool present=false;const XrXirValue *object=NULL;bool nullable=xr_xir_nullable_view(result,&present,&object);
    fprintf(stderr,"complete os.exec command=%s\nchild pid%lu completed%lu stdout%llu stderr%llu ready%lu done%lu handle%u wait%lu exitValid%u exit%lu\nresult type%u nullable%u Some%u\n",
        command,(unsigned long)child->record->pid,(unsigned long)child->record->completed,
        (unsigned long long)child->record->output_length,(unsigned long long)child->record->error_length,
        (unsigned long)WaitForSingleObject(child->ready,0),(unsigned long)WaitForSingleObject(child->done,0),(unsigned)(child->process!=NULL),
        (unsigned long)(child->process?WaitForSingleObject(child->process,0):WAIT_FAILED),(unsigned)(queried!=FALSE),(unsigned long)exit_code,
        result->type,(unsigned)nullable,(unsigned)present);
    if(!nullable||!present||!object)return;
    XrXirValue fields={0},values[5]={{0}};XrXirCallStatus status=xr_xir_instance_start(instance,ids[PROCESS_COMPLETE_OS_FIELDS],object,1);
    if(status==XR_XIR_CALL_READY){status=process_drive(instance).outcome.status;if(status==XR_XIR_CALL_RETURNED)status=xr_xir_instance_take_result(instance,&fields);}
    fprintf(stderr,"complete os.exec fields status%u\n",(unsigned)status);
    if(status==XR_XIR_CALL_RETURNED){
        for(unsigned n=0;n<5;++n){
            XrXirValueStatus projected=xr_xir_tuple_get(&fields,n,&values[n]);
            fprintf(stderr,"complete os.exec field%u status%u type%u",n,(unsigned)projected,values[n].type);
            if(values[n].type==XR_XIR_I64||values[n].type==XR_XIR_BOOL)fprintf(stderr," scalar%lld",(long long)values[n].payload);fputc('\n',stderr);
            if(projected==XR_XIR_VALUE_OK&&n<2)process_complete_os_diagnostic_bytes(instance,&values[n],n?"stderr":"stdout");
        }
    }
    for(unsigned n=0;n<5;++n)xr_xir_value_drop(&values[n]);xr_xir_value_drop(&fields);
}
static void process_complete_os_success(XrXirProgram *program,const uint32_t *ids,const int64_t limits[2],bool spaced_argument){
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=process_instance(program,ids);ProcessCompleteOsChild child;process_complete_os_open(&child,true);
    child.spaced_argument=spaced_argument;
    char *command=process_complete_os_command(&child);XrXirValue text=process_string(instance,command);
    XrXirValue args[]={text,{XR_XIR_I64,0,limits[0]},{XR_XIR_I64,0,limits[1]}};process_complete_os_arm(false);
    CHECK(xr_xir_instance_start(instance,ids[PROCESS_COMPLETE_OS_EXEC],args,3)==XR_XIR_CALL_READY);
    CHECK(process_complete_os_drive(instance,&child)==XR_XIR_CALL_RETURNED);XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    bool complete=child.process&&child.record->completed&&WaitForSingleObject(child.done,0)==WAIT_OBJECT_0;
    if(!complete)process_complete_os_diagnostic(instance,ids,&result,&child,command);
    CHECK(complete);xr_free(command);process_child_done(child.process,23,true);
    CHECK(process_complete_os_acquired&&process_complete_os_acquired==process_complete_os_retired&&process_oom_prepares==1&&process_oom_starts==1);
    CHECK(process_complete_os_closed_ends[0]==2&&process_complete_os_closed_ends[1]==2);
    uint32_t close_native=UINT32_MAX,spawn_native=UINT32_MAX;for(uint32_t f=0;f<program->entry_count;++f){
        if(program->declarations->functions[f].native_callable==109)close_native=f;
        if(program->declarations->functions[f].native_callable==110)spawn_native=f;
    }
    XrXirValue absent[]={{XR_XIR_I64,0,-1},{XR_XIR_I64,0,-1}},shell_args[]={text,absent[0],absent[1]};
    CHECK(close_native!=UINT32_MAX&&xr_xir_instance_start(instance,close_native,absent,2)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(spawn_native!=UINT32_MAX&&xr_xir_instance_start(instance,spawn_native,shell_args,3)==XR_XIR_CALL_BAD_ARGUMENT);
    process_complete_os_result(instance,ids,&result,&child,limits);xr_xir_value_drop(&result);xr_xir_value_drop(&text);
    unsigned acquired=process_complete_os_acquired;process_retire(instance);CHECK(process_complete_os_acquired==acquired);process_complete_os_disarm();
    process_complete_os_close(&child);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_complete_os_refusals(XrXirProgram *program,const uint32_t *ids){
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=process_instance(program,ids);XrXirValue text=process_string(instance,"");
    const int64_t limits[][2]={{1,1},{-1,1},{1,-1},{INT64_MAX,1},{1,INT64_MAX}};process_complete_os_arm(false);
    for(unsigned n=0;n<sizeof(limits)/sizeof(*limits);++n){
        if(n==1){xr_xir_value_drop(&text);text=process_string(instance,"this-command-must-not-start");}
        XrXirValue args[]={text,{XR_XIR_I64,0,limits[n][0]},{XR_XIR_I64,0,limits[n][1]}};
        XrXirValue result=process_invoke(instance,ids[PROCESS_COMPLETE_OS_EXEC],args,3);process_some(&result,false);xr_xir_value_drop(&result);
    }
    CHECK(!process_oom_prepares&&!process_oom_starts&&!process_complete_os_acquired);xr_xir_value_drop(&text);process_retire(instance);process_complete_os_disarm();CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_complete_os_tokens(XrXirInstance *instance,const uint32_t *ids,int64_t tokens[2]){
    XrXirValue opened=process_invoke(instance,ids[PROCESS_COMPLETE_OS_OPEN],NULL,0);const XrXirValue *tuple=process_some(&opened,true);
    for(unsigned n=0;n<2;++n){
        XrXirValue token={0};CHECK(xr_xir_tuple_get(tuple,n,&token)==XR_XIR_VALUE_OK&&token.type==XR_XIR_I64&&token.payload>0);
        tokens[n]=token.payload;xr_xir_value_drop(&token);
    }
    CHECK(tokens[0]!=tokens[1]);xr_xir_value_drop(&opened);
}
static void process_complete_os_cleanup(XrXirInstance *instance,uint32_t entry,int64_t read,int64_t write,bool expected){
    XrXirValue args[]={{XR_XIR_I64,0,read},{XR_XIR_I64,0,write}},result=process_invoke(instance,entry,args,2);
    CHECK(result.type==XR_XIR_BOOL&&result.payload==expected);xr_xir_value_drop(&result);
}
static void process_complete_os_cleanup_refusals(XrXirProgram *program,const uint32_t *ids){
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *a=process_instance(program,ids),*b=process_instance(program,ids);
    int64_t first[2]={0},second[2]={0};process_complete_os_tokens(a,ids,first);process_complete_os_tokens(b,ids,second);
    for(unsigned n=0;n<2;++n)for(unsigned m=0;m<2;++m)CHECK(first[n]!=second[m]);
    CHECK(first[0]!=INT64_MAX&&first[1]!=INT64_MAX);process_complete_os_arm(false);
    process_complete_os_cleanup(b,ids[PROCESS_COMPLETE_OS_CLOSE],first[0],first[1],false);
    process_complete_os_cleanup(a,ids[PROCESS_COMPLETE_OS_CLOSE],first[1],first[0],false);
    process_complete_os_cleanup(a,ids[PROCESS_COMPLETE_OS_CLOSE],INT64_MAX,-1,false);
    CHECK(!process_complete_os_close_attempts[0]&&!process_complete_os_close_attempts[1]);
    /* An unknown first endpoint cannot suppress closing the valid second one. */
    process_complete_os_cleanup(a,ids[PROCESS_COMPLETE_OS_CLOSE],INT64_MAX,first[1],false);
    CHECK(!process_complete_os_close_attempts[0]&&process_complete_os_close_attempts[1]==1&&process_complete_os_closed_ends[1]==1);
    process_complete_os_cleanup(a,ids[PROCESS_COMPLETE_OS_ORDINARY_CLOSE],-1,first[1],false);
    process_complete_os_cleanup(a,ids[PROCESS_COMPLETE_OS_CLOSE],first[0],-1,true);
    CHECK(process_complete_os_close_attempts[0]==1&&process_complete_os_close_attempts[1]==1);
    /* A genuinely closed first endpoint is stale, rather than absent. */
    process_complete_os_cleanup(b,ids[PROCESS_COMPLETE_OS_ORDINARY_CLOSE],second[0],-1,true);
    process_complete_os_cleanup(b,ids[PROCESS_COMPLETE_OS_CLOSE],second[0],second[1],false);
    CHECK(process_complete_os_close_attempts[0]==2&&process_complete_os_close_attempts[1]==2&&
        process_complete_os_closed_ends[0]==2&&process_complete_os_closed_ends[1]==2);
    process_complete_os_cleanup(b,ids[PROCESS_COMPLETE_OS_CLOSE],second[0],second[1],false);
    process_complete_os_cleanup(b,ids[PROCESS_COMPLETE_OS_CLOSE],-1,-1,true);
    CHECK(process_complete_os_close_attempts[0]==2&&process_complete_os_close_attempts[1]==2);
    process_retire(a);process_retire(b);process_complete_os_disarm();CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_complete_os_cleanup_fault(XrXirProgram *program,const uint32_t *ids){
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=process_instance(program,ids);int64_t tokens[2]={0};process_complete_os_tokens(instance,ids,tokens);
    XrXirValue args[]={{XR_XIR_I64,0,tokens[0]},{XR_XIR_I64,0,tokens[1]}};process_complete_os_arm(false);process_complete_os_fail_close=true;
    CHECK(xr_xir_instance_start(instance,ids[PROCESS_COMPLETE_OS_CLOSE],args,2)==XR_XIR_CALL_READY);
    CHECK(process_drive(instance).outcome.status==XR_XIR_CALL_HOST_ERROR);XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_HOST_ERROR&&!result.type&&!result.reserved&&!result.payload);
    CHECK(process_complete_os_close_faults==1&&!process_complete_os_fail_close&&
        process_complete_os_close_attempts[0]==1&&process_complete_os_close_attempts[1]==1&&
        !process_complete_os_closed_ends[0]&&process_complete_os_closed_ends[1]==1&&!process_complete_os_disposals);
    /* The failed read close remains a Root duty. Retirement never replays the guest. */
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;){
        XrXirCallStatus status=xr_xir_instance_free(instance);
        if(status!=XR_XIR_CALL_CLOSE_FAILED){CHECK(status==XR_XIR_CALL_HOST_ERROR);break;}
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));xr_thread_yield();
    }
    CHECK(process_complete_os_disposals==1&&process_complete_os_close_faults==1&&
        process_complete_os_close_attempts[0]==1&&process_complete_os_close_attempts[1]==1);
    process_complete_os_disarm();CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_complete_os_failure(XrXirProgram *program,const uint32_t *ids,bool cancel){
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=process_instance(program,ids);ProcessCompleteOsChild child;process_complete_os_open(&child,!cancel);
    char *command=process_complete_os_command(&child);XrXirValue text=process_string(instance,command);xr_free(command);
    XrXirValue args[]={text,{XR_XIR_I64,0,2147483647},{XR_XIR_I64,0,2147483647}};process_complete_os_arm(!cancel);
    CHECK(xr_xir_instance_start(instance,ids[PROCESS_COMPLETE_OS_EXEC],args,3)==XR_XIR_CALL_READY);
    if(cancel){
        uint64_t started=xr_time_monotonic_ns();CHECK(started);
        while(!child.process||!process_complete_os_pending){
            XrXirCallStatus status=process_complete_os_step(instance,&child).outcome.status;CHECK(status==XR_XIR_CALL_READY||status==XR_XIR_CALL_SUSPENDED);
            uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));
        }
        CHECK(xr_xir_call_request_cancel(instance->call)==XR_XIR_CALL_CANCEL_REQUESTED);
    }
    XrXirCallStatus status=process_complete_os_drive(instance,&child);runtime_fail_at=SIZE_MAX;
    if(status!=(cancel?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_OOM))fprintf(stderr,"complete os.exec cancel%u status%u\n",(unsigned)cancel,status);
    CHECK(status==(cancel?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_OOM)&&child.process);
    CHECK(process_complete_os_acquired&&process_complete_os_acquired==process_complete_os_retired);
    if(!cancel)CHECK(process_complete_os_hits==1&&!process_complete_os_oom&&runtime_attempts>process_complete_os_allocation);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==(cancel?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_OOM)&&!result.type&&!result.reserved&&!result.payload);
    DWORD before_root_free=WaitForSingleObject(child.process,0);CHECK(before_root_free==WAIT_OBJECT_0||before_root_free==WAIT_TIMEOUT);
    if(getenv("XR_PROCESS_TRACE"))fprintf(stderr,"complete os.exec activation terminal: childTerminated=%u, operationsRetired=%u\n",(unsigned)(before_root_free==WAIT_OBJECT_0),process_complete_os_retired);
    xr_xir_value_drop(&text);unsigned acquired=process_complete_os_acquired;uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;){
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        if(closed!=XR_XIR_CALL_CLOSE_FAILED){CHECK(closed==(cancel?XR_XIR_CALL_READY:XR_XIR_CALL_OOM));break;}
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));xr_thread_yield();
    }
    CHECK(process_complete_os_acquired==acquired&&process_oom_prepares==1&&process_oom_starts==1);process_complete_os_disarm();
    process_child_done(child.process,0,false);process_complete_os_close(&child);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_complete_os_execute(XrXirProgram *program,const uint32_t *ids){
    const int64_t limits[][2]={{2147483647,2147483647},{7,11},{0,0}};
    process_trace("complete os.exec capture enter");
    for(unsigned n=0;n<sizeof(limits)/sizeof(*limits);++n)process_complete_os_success(program,ids,limits[n],false);
    process_complete_os_success(program,ids,limits[1],true);
    process_trace("complete os.exec capture exit");process_complete_os_refusals(program,ids);
    process_trace("complete os.exec cleanup refusals enter");process_complete_os_cleanup_refusals(program,ids);process_trace("complete os.exec cleanup refusals exit");
    process_trace("complete os.exec cleanup fault enter");process_complete_os_cleanup_fault(program,ids);process_trace("complete os.exec cleanup fault exit");
    process_trace("complete os.exec cancel enter");process_complete_os_failure(program,ids,true);process_trace("complete os.exec cancel exit");
    process_trace("complete os.exec OOM enter");process_complete_os_failure(program,ids,false);process_trace("complete os.exec OOM exit");
}
#else
static bool process_complete_os_child(int argc,char **argv){(void)argc;(void)argv;return false;}
static void process_complete_os_execute(XrXirProgram *program,const uint32_t *ids){
    (void)program;(void)ids;fprintf(stderr,"Complete os.exec qualification requires the Windows Process provider\n");exit(2);
}
#endif
#endif // PROCESS_COMPLETE_OS_RUNTIME_H
