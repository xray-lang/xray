/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_dap_process_win.inc.c - Owned stdio worker and irrevocable job handoff
 */
#include "../../os/os_proc.h"
#include "../../os/os_random.h"
#include "../../base/xwindows_utf8.h"
#include "../../base/xchecks.h"
#include <io.h>
#include <string.h>

typedef struct DapProcess {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    HANDLE events[3],parent,files[2];
    char *paths[2];
    bool released;
} DapProcess;
static bool dap_process_work(XrCompileResources *resources,uint64_t units) {
    return xr_compile_resources_work(resources,units)==XR_COMPILE_RESOURCE_OK;
}
static XrOsProcStatus dap_process_alloc(void *context,size_t bytes,void **output) {
    XrCompileResourceStatus status=xr_compile_resources_alloc(context,bytes,output);
    return status==XR_COMPILE_RESOURCE_OK ? XR_PROC_OK : status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_PROC_OUT_OF_MEMORY : XR_PROC_BUDGET;
}
static void dap_process_free(void *context,void *memory) { (void)context;xr_compile_resources_free(memory); }
static XrOsProcStatus dap_process_charge(void *context,uint64_t units) {
    return dap_process_work(context,units) ? XR_PROC_OK : XR_PROC_BUDGET;
}
static bool dap_process_handle_close(HANDLE *slot) {
    HANDLE handle=*slot;*slot=NULL;
    return !handle || handle==INVALID_HANDLE_VALUE || CloseHandle(handle)!=0;
}
static bool dap_process_event_names(const char *nonce,wchar_t names[3][112]) {
    if (!nonce) return false;
    for (size_t i=0;i<64;++i)
        if (!((nonce[i]>='0' && nonce[i]<='9') || (nonce[i]>='a' && nonce[i]<='f'))) return false;
    if (nonce[64]) return false;
    for (unsigned i=0;i<3;++i) {
        const wchar_t prefix[]=L"Local\\XrayDap-";size_t used=0;
        while (prefix[used]) { names[i][used]=prefix[used];++used; }
        for (size_t j=0;j<64;++j) names[i][used++]=(wchar_t)(unsigned char)nonce[j];
        names[i][used++]=L'-';names[i][used++]=(wchar_t)(L'0'+i);names[i][used]=0;
        XR_DCHECK(used<112,"Authenticated event names fit their owned fixed storage");
    }
    return true;
}
/* The explicit user DACL is also inherited by retained output files. Named
 * controls are not inherited handles; only the worker knowing the nonce opens them. */
static bool dap_process_security(SECURITY_ATTRIBUTES *attributes,SECURITY_DESCRIPTOR *descriptor,
    unsigned char *acl_bytes,unsigned char *user_bytes) {
    HANDLE token=NULL;DWORD needed=0;bool okay=false;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
    if (GetTokenInformation(token,TokenUser,user_bytes,256,&needed)) {
        PSID sid=((TOKEN_USER *)(void *)user_bytes)->User.Sid;PACL acl=(PACL)(void *)acl_bytes;
        okay=IsValidSid(sid) && InitializeAcl(acl,256,ACL_REVISION) &&
            AddAccessAllowedAceEx(acl,ACL_REVISION,OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE,GENERIC_ALL,sid) &&
            InitializeSecurityDescriptor(descriptor,SECURITY_DESCRIPTOR_REVISION) &&
            SetSecurityDescriptorDacl(descriptor,TRUE,acl,FALSE) &&
            SetSecurityDescriptorControl(descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED);
    }
    if (!CloseHandle(token)) okay=false;
    *attributes=(SECURITY_ATTRIBUTES){sizeof(*attributes),descriptor,FALSE};return okay;
}
static XrXirStatus dap_process_path(const XrOsIoPolicy *policy,const char *directory,const char *leaf,char **output) {
    XrIoContext io={policy,XR_OS_IO_OK};size_t length=0,tail=0;
    if (!io_length(&io,directory,&length) || !io_length(&io,leaf,&tail))
        return io.status==XR_OS_IO_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    if (length>131000 || tail>32 || policy->work(policy->context,1+length+tail)!=XR_OS_IO_OK) return XR_XIR_BUDGET;
    char *text=NULL;XrOsIoStatus status=policy->alloc(policy->context,length+tail+2,(void **)&text);
    if (status!=XR_OS_IO_OK) return status==XR_OS_IO_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(text,directory,length);text[length]='\\';memcpy(text+length+1,leaf,tail+1);*output=text;return XR_XIR_OK;
}
static XrXirStatus dap_process_file(const XrOsIoPolicy *policy,const char *path,HANDLE *output) {
    wchar_t *wide=NULL;XrOsIoStatus status=xr_win_utf8_path_owned(policy,path,&wide);
    if (status!=XR_OS_IO_OK) return status==XR_OS_IO_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status==XR_OS_IO_BUDGET ? XR_XIR_BUDGET : XR_XIR_IO;
    XR_DCHECK(wide && *wide,"An admitted retained file has a nonempty owned locator");
    HANDLE handle=INVALID_HANDLE_VALUE;
    if (policy->work(policy->context,1)!=XR_OS_IO_OK) status=XR_OS_IO_BUDGET;
    else handle=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    policy->free(policy->context,wide);
    if (status!=XR_OS_IO_OK) return XR_XIR_BUDGET;
    if (handle==INVALID_HANDLE_VALUE) return XR_XIR_IO;
    *output=handle;return XR_XIR_OK;
}
static XrXirOutputStatus dap_process_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    DapProcess *process=context;
    if (!process || (stream!=XR_XIR_STDOUT && stream!=XR_XIR_STDERR) || (!bytes && length)) return XR_XIR_OUTPUT_BAD_ARGUMENT;
    HANDLE file=process->files[stream==XR_XIR_STDERR ? 1 : 0];
    while (length) {
        DWORD request=length>UINT32_MAX ? UINT32_MAX : (DWORD)length,written=0;
        if (!dap_process_work(process->resources,1+request)) return XR_XIR_OUTPUT_LIMIT;
        if (!WriteFile(file,bytes,request,&written,NULL) || !written) return XR_XIR_OUTPUT_ERROR;
        bytes+=written;length-=written;
    }
    return XR_XIR_OUTPUT_OK;
}
static XrXirCallStatus dap_process_handoff(void *context) {
    DapProcess *process=context;
    if (!process || process->released) return XR_XIR_CALL_BAD_STATE;
    /* Prepay the fixed handshake. There is no fallible admission between the
     * host's irreversible release and observation retirement in the worker. */
    if (!dap_process_work(process->resources,512)) return XR_XIR_CALL_LIMIT;
    if (!FlushFileBuffers(process->files[0]) || !FlushFileBuffers(process->files[1]) ||
        !ResetEvent(process->events[1]) || !ResetEvent(process->events[2]) || !SetEvent(process->events[0]))
        return XR_XIR_CALL_HOST_ERROR;
    HANDLE decisions[]={process->events[1],process->events[2],process->parent};
    DWORD decision=WaitForMultipleObjects(3,decisions,FALSE,INFINITE);
    XR_DCHECK(decision<=WAIT_OBJECT_0+2 || decision==WAIT_FAILED,"Only a control decision or parent exit settles handoff");
    bool committed=decision==WAIT_OBJECT_0;
    if (decision==WAIT_OBJECT_0+2) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
        /* A frontend can die after clearing the job policy and before sending
         * its acknowledgement. The actual job, not pipe EOF, settles that race. */
        if (!QueryInformationJobObject(NULL,JobObjectExtendedLimitInformation,&limits,sizeof(limits),NULL))
            return XR_XIR_CALL_HOST_ERROR;
        committed=(limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)==0;
    }
    if (decision==WAIT_OBJECT_0+1) return XR_XIR_CALL_BAD_STATE;
    if (!committed) return XR_XIR_CALL_HOST_ERROR;
    process->released=true;return XR_XIR_CALL_READY;
}
static XrXirStatus dap_process_retire(void *context) {
    DapProcess *process=context;if (!process || !process->released) return XR_XIR_BAD_STRUCTURE;
    bool okay=SetStdHandle(STD_INPUT_HANDLE,NULL)!=0;
    if (!SetStdHandle(STD_OUTPUT_HANDLE,NULL)) okay=false;
    if (fclose(stdin)) okay=false;
    if (fclose(stdout)) okay=false;
    return okay ? XR_XIR_OK : XR_XIR_IO;
}
static XrXirStatus dap_process_worker_close(DapHost *host) {
    DapProcess *process=host->context;if (!process) return XR_XIR_OK;
    bool okay=true;
    for (unsigned i=0;i<2;++i) {
        if (process->files[i] && !FlushFileBuffers(process->files[i])) okay=false;
        if (!dap_process_handle_close(&process->files[i])) okay=false;
        xr_compile_resources_free(process->paths[i]);
    }
    for (unsigned i=0;i<3;++i) if (!dap_process_handle_close(&process->events[i])) okay=false;
    if (!dap_process_handle_close(&process->parent)) okay=false;
    xr_compile_resources_free(process);*host=(DapHost){0};return okay ? XR_XIR_OK : XR_XIR_IO;
}
static XrXirStatus dap_process_worker_open(XrCompileResources *resources,int argc,char **argv,DapHost *host) {
    if (argc!=5 || !argv || !host || host->context || !dap_process_work(resources,1024)) return XR_XIR_BAD_STRUCTURE;
    uint64_t parent=0;size_t digits=0;
    for (;argv[2][digits];++digits) {
        char c=argv[2][digits];if (digits>=10 || c<'0' || c>'9') return XR_XIR_BAD_STRUCTURE;
        parent=parent*10+(uint64_t)(c-'0');if (parent>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    }
    wchar_t names[3][112];JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    if (GetConsoleWindow()!=NULL || !parent || parent==GetCurrentProcessId() || !dap_process_event_names(argv[3],names) ||
        !QueryInformationJobObject(NULL,JobObjectExtendedLimitInformation,&limits,sizeof(limits),NULL) ||
        !(limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) ||
        (HANDLE)_get_osfhandle(_fileno(stdin))!=GetStdHandle(STD_INPUT_HANDLE) ||
        (HANDLE)_get_osfhandle(_fileno(stdout))!=GetStdHandle(STD_OUTPUT_HANDLE)) return XR_XIR_BAD_STRUCTURE;
    DapProcess *process=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_calloc(resources,1,sizeof(*process),(void **)&process);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    process->resources=resources;process->policy=xr_compile_io_policy(resources);host->context=process;
    process->parent=OpenProcess(SYNCHRONIZE,FALSE,(DWORD)parent);
    if (!process->parent) return XR_XIR_IO;
    for (unsigned i=0;i<3;++i) {
        process->events[i]=OpenEventW(i==0 ? EVENT_MODIFY_STATE : SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,names[i]);
        if (!process->events[i]) return XR_XIR_IO;
    }
    const char *leaves[]={"stdout.bin","stderr.bin"};
    for (unsigned i=0;i<2;++i) {
        XrXirStatus status=dap_process_path(&process->policy,argv[4],leaves[i],&process->paths[i]);
        if (status==XR_XIR_OK) status=dap_process_file(&process->policy,process->paths[i],&process->files[i]);
        if (status!=XR_XIR_OK) return status;
    }
    host->commit=dap_process_handoff;host->retire_control=dap_process_retire;
    host->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,dap_process_output,process,1048576};
    host->stdout_path=process->paths[0];host->stderr_path=process->paths[1];host->process_id=GetCurrentProcessId();return XR_XIR_OK;
}
static XrXirStatus dap_process_directory(XrCompileResources *resources,const char *nonce,
    SECURITY_ATTRIBUTES *security,char **output) {
    wchar_t *wide=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_alloc(resources,32768*sizeof(*wide),(void **)&wide);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_IO;XrOsIoPolicy policy=xr_compile_io_policy(resources);
    if (!dap_process_work(resources,65536+512)) { status=XR_XIR_BUDGET;goto done; }
    DWORD length=GetTempPathW(32768,wide);
    if (!length || length>32660) goto done;
    const wchar_t prefix[]=L"xray-dap-";
    for (size_t i=0;i<sizeof(prefix)/sizeof(*prefix)-1;++i) wide[length++]=prefix[i];
    for (size_t i=0;i<64;++i) wide[length++]=(wchar_t)(unsigned char)nonce[i];
    wide[length]=0;
    XrOsIoStatus located=xr_win_path_locator_owned(&policy,&wide,length);
    if (located!=XR_OS_IO_OK) { status=located==XR_OS_IO_BUDGET ? XR_XIR_BUDGET :
        located==XR_OS_IO_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_IO;goto done; }
    if (!CreateDirectoryW(wide,security)) goto done;
    XrOsIoStatus converted=xr_win_utf16_text_owned(&policy,wide,output);
    status=converted==XR_OS_IO_OK ? XR_XIR_OK : converted==XR_OS_IO_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        converted==XR_OS_IO_BUDGET ? XR_XIR_BUDGET : XR_XIR_IO;
done:
    xr_compile_resources_free(wide);return status;
}
static int dap_process_frontend(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    if (xr_compile_resources_new(&limits,&resources)!=XR_COMPILE_RESOURCE_OK) return XR_CLI_EXIT_INTERNAL;
    XrOsIoPolicy policy=xr_compile_io_policy(resources);XrProcId owner=XR_PROC_INVALID;
    HANDLE events[3]={0},diagnostic=NULL;char *directory=NULL,*log=NULL,*executable=NULL;
    unsigned char entropy[32];union { TOKEN_USER alignment;ACL header;unsigned char bytes[256]; } acl,user;
    SECURITY_DESCRIPTOR descriptor;
    SECURITY_ATTRIBUTES security;wchar_t names[3][112];char nonce[65],parent[16];int result=XR_CLI_EXIT_FAIL;
    if (!dap_process_work(resources,4096)) goto done;
    xr_random_bytes(entropy,sizeof(entropy));
    for (size_t i=0;i<32;++i) { nonce[i*2]="0123456789abcdef"[entropy[i]>>4];nonce[i*2+1]="0123456789abcdef"[entropy[i]&15]; }
    nonce[64]=0;
    if (!dap_process_event_names(nonce,names) || !dap_process_security(&security,&descriptor,acl.bytes,user.bytes)) goto done;
    for (unsigned i=0;i<3;++i) {
        events[i]=CreateEventW(&security,TRUE,FALSE,names[i]);
        if (!events[i] || GetLastError()==ERROR_ALREADY_EXISTS) goto done;
    }
    if (dap_process_directory(resources,nonce,&security,&directory)!=XR_XIR_OK ||
        dap_process_path(&policy,directory,"host.log",&log)!=XR_XIR_OK ||
        dap_process_file(&policy,log,&diagnostic)!=XR_XIR_OK ||
        xr_os_io_self_exe_path(&policy,&executable)!=XR_OS_IO_OK) goto done;
    if (snprintf(parent,sizeof(parent),"%lu",(unsigned long)GetCurrentProcessId())<=0) goto done;
    const char *arguments[]={executable,"--xray-dap-worker",parent,nonce,directory,NULL};
    XrProcSpawnOptions spawn={0};spawn.memory=(XrProcMemory){resources,dap_process_alloc,dap_process_free,dap_process_charge};
    spawn.new_process_group=true;spawn.no_console_window=true;spawn.has_stdin=spawn.has_stdout=spawn.has_stderr=true;
    spawn.stdin_read=(XrPipeHandle)(intptr_t)GetStdHandle(STD_INPUT_HANDLE);
    spawn.stdout_write=(XrPipeHandle)(intptr_t)GetStdHandle(STD_OUTPUT_HANDLE);spawn.stderr_write=(XrPipeHandle)(intptr_t)diagnostic;
    if (xr_proc_spawn(executable,arguments,&spawn,&owner)!=XR_PROC_OK) goto done;
    XR_DCHECK(owner!=XR_PROC_INVALID,"Successful worker spawn publishes its unique group owner");
    if (!dap_process_handle_close(&diagnostic)) goto done;
    while (true) {
        if (!dap_process_work(resources,2)) goto done;
        DWORD requested=WaitForSingleObject(events[0],10);
        if (requested==WAIT_OBJECT_0) {
            if (!ResetEvent(events[0])) goto done;
            XrOsProcStatus released=xr_proc_release(&owner,&spawn.memory);
            bool committed=owner==XR_PROC_INVALID;
            XR_DCHECK(committed || released!=XR_PROC_OK,"Successful host release consumes its original owner");
            if (!SetEvent(events[committed ? 1 : 2])) goto done;
            if (committed) { result=released==XR_PROC_OK ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;goto done; }
        } else if (requested!=WAIT_TIMEOUT) goto done;
        int exit_code=-1;XrProcWaitResult waited=xr_proc_try_wait(owner,&exit_code);
        if (waited==XR_PROC_WAIT_ERROR) goto done;
        if (waited==XR_PROC_WAIT_EXITED) { result=exit_code==0 ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;goto done; }
    }
done:
    if (owner!=XR_PROC_INVALID && xr_proc_close(owner)) result=XR_CLI_EXIT_FAIL;
    for (unsigned i=0;i<3;++i) if (!dap_process_handle_close(&events[i])) result=XR_CLI_EXIT_FAIL;
    if (!dap_process_handle_close(&diagnostic)) result=XR_CLI_EXIT_FAIL;
    if (result!=XR_CLI_EXIT_OK) {
        fputs("DAP: protocol, source admission, resource or cleanup failure\n",stderr);
        if (log) fprintf(stderr,"DAP: host diagnostics: %s\n",log);
    }
    xr_compile_resources_free(executable);xr_compile_resources_free(log);xr_compile_resources_free(directory);
    xr_compile_resources_release(resources);return result;
}
