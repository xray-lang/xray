/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_handoff_win.inc.c - Explicit non-terminating release of an owned job
 */
#include "../../base/xchecks.h"
XR_FUNC XrOsProcStatus xr_proc_release(XrProcId *owner) {
    if (!owner || *owner<=0 || (uint64_t)*owner>UINT32_MAX) return XR_PROC_INVALID_ARGUMENT;
    XrProcId pid=*owner;
    AcquireSRWLockExclusive(&g_live_lock);ProcLive *record=proc_find(pid);
    if (!record || !record->job || record->releasing) {
        ReleaseSRWLockExclusive(&g_live_lock);return XR_PROC_INVALID_ARGUMENT;
    }
    if (record->debug) { ReleaseSRWLockExclusive(&g_live_lock);return XR_PROC_UNSUPPORTED; }
    record->releasing=true;
    HANDLE job=record->job;ReleaseSRWLockExclusive(&g_live_lock);
    /* The reservation keeps both handles owned while OS calls run without
     * the registry lock. Reentrant and competing consumers must be refused. */
    XrOsProcStatus status=XR_PROC_OK;
    ProcLive released={0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    if (status==XR_PROC_OK && !QueryInformationJobObject(job,JobObjectExtendedLimitInformation,
        &limits,sizeof(limits),NULL)) status=xr_proc_last_error();
    if (status==XR_PROC_OK) {
        limits.BasicLimitInformation.LimitFlags&=~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            status=xr_proc_last_error();
    }
    AcquireSRWLockExclusive(&g_live_lock);record=proc_find(pid);
    XR_CHECK(record && record->releasing && record->job==job,"Reserved group ownership is exclusive until release settles");
    if (status==XR_PROC_OK) {
        released=*record;memset(record,0,sizeof(*record));*owner=XR_PROC_INVALID;
    } else record->releasing=false;
    ReleaseSRWLockExclusive(&g_live_lock);
    if (status!=XR_PROC_OK) return status;
    /* Revoking kill-on-close is the irreversible transfer boundary. Report a
     * cleanup error with the consumed owner instead of inviting a target kill. */
    bool job_closed=CloseHandle(released.job)!=0,process_closed=CloseHandle(released.process)!=0;
    return job_closed && process_closed ? XR_PROC_OK : XR_PROC_IO;
}
