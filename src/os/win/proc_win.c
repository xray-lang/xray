/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_win.c - Windows implementation of os_proc.h.
 *
 * Numeric PIDs map to retained process and optional job handles. Ordinary
 * waits consume their record; group records survive leader exit until close
 * terminates descendants and releases the job. Waiters lease independent
 * handles and serialize the single successful consumption of ordinary IDs.
 */

#include "../os_proc.h"
#include "../../base/xmalloc.h"
#include "../../shared/xr_win_utf.h"
#include <string.h>
#include <limits.h>

#define XR_PROC_MAX_LIVE 64
#define XR_PROC_KILLED_EXIT_CODE ((DWORD) 0xE0000001u)
typedef struct ProcLive { DWORD pid; HANDLE process, job; bool waited; } ProcLive;
static ProcLive g_live[XR_PROC_MAX_LIVE];
static SRWLOCK g_live_lock = SRWLOCK_INIT;

static XrOsProcStatus proc_system_alloc(void *context, size_t bytes, void **out) {
    (void) context; void *p = xr_malloc(bytes);
    if (!p) return XR_PROC_OUT_OF_MEMORY;
    *out = p; return XR_PROC_OK;
}
static void proc_system_free(void *context, void *p) { (void) context; xr_free(p); }
static XrOsProcStatus proc_system_work(void *context, uint64_t n) {
    (void) context; (void) n; return XR_PROC_OK;
}
XR_FUNC XrProcMemory xr_proc_system_memory(void) {
    XrProcMemory m = {NULL, proc_system_alloc, proc_system_free, proc_system_work}; return m;
}
XR_FUNC XrOsProcStatus xr_proc_last_error(void) {
    DWORD e = GetLastError();
    if (e == ERROR_NOT_ENOUGH_MEMORY || e == ERROR_OUTOFMEMORY) return XR_PROC_OUT_OF_MEMORY;
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY) return XR_PROC_UNRESOLVED;
    if (e == ERROR_INVALID_PARAMETER || e == ERROR_NO_UNICODE_TRANSLATION) return XR_PROC_INVALID_ARGUMENT;
    return XR_PROC_IO;
}
typedef struct ProcBuild { XrProcMemory memory; XrOsProcStatus status; } ProcBuild;
static bool proc_work(ProcBuild *b, uint64_t n) {
    if (b->status == XR_PROC_OK) b->status = b->memory.work(b->memory.context, n);
    return b->status == XR_PROC_OK;
}
static void *proc_alloc(ProcBuild *b, size_t n) {
    void *p = NULL;
    if (b->status == XR_PROC_OK) b->status = b->memory.alloc(b->memory.context, n, &p);
    return p;
}
static void proc_free(ProcBuild *b, void *p) { if (p) b->memory.free(b->memory.context, p); }
static bool proc_length(ProcBuild *b, const char *s, size_t *n) {
    if (!s) { b->status = XR_PROC_INVALID_ARGUMENT; return false; }
    *n = 0;
    for (;;) {
        if (!proc_work(b, 1)) return false;
        if (!s[*n]) return true;
        if (*n == INT_MAX) { b->status = XR_PROC_BUDGET; return false; }
        ++*n;
    }
}
static wchar_t *proc_wide(ProcBuild *b, const char *s) {
    size_t n;
    if (!proc_length(b, s, &n) || !proc_work(b, n + 1)) return NULL;
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n + 1, NULL, 0);
    if (!units) { b->status = xr_proc_last_error(); return NULL; }
    wchar_t *out = proc_alloc(b, (size_t)units * sizeof(wchar_t));
    if (!out) return NULL;
    if (!proc_work(b, n + 1 + (uint64_t)units * sizeof(wchar_t)) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n + 1, out, units)) {
        if (b->status == XR_PROC_OK) b->status = xr_proc_last_error();
        proc_free(b, out); return NULL;
    }
    return out;
}
static bool proc_emit(ProcBuild *b, wchar_t *out, size_t *n, wchar_t value) {
    if (!proc_work(b, sizeof(wchar_t))) return false;
    out[(*n)++] = value; return true;
}
static wchar_t *proc_command(ProcBuild *b, const char *const *argv) {
    size_t cap = 1, count = 0;
    for (; argv[count]; ++count) {
        size_t n;
        if (!proc_length(b, argv[count], &n)) return NULL;
        if (n > (SIZE_MAX - cap - 3) / 2) { b->status = XR_PROC_BUDGET; return NULL; }
        cap += n * 2 + 3;
    }
    if (!count || cap > SIZE_MAX / sizeof(wchar_t)) { b->status = XR_PROC_INVALID_ARGUMENT; return NULL; }
    wchar_t *out = proc_alloc(b, cap * sizeof(wchar_t)); size_t pos = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < count && b->status == XR_PROC_OK; ++i) {
        wchar_t *arg = proc_wide(b, argv[i]); if (!arg) break;
        size_t n = 0; bool quotes = !arg[0];
        while (proc_work(b, sizeof(wchar_t))) {
            if (!arg[n]) break;
            quotes = quotes || arg[n] == L' ' || arg[n] == L'\t' || arg[n] == L'"'; ++n;
        }
        if (i) proc_emit(b, out, &pos, L' ');
        if (quotes) proc_emit(b, out, &pos, L'"');
        size_t slash = 0;
        for (size_t j = 0; j < n && b->status == XR_PROC_OK; ++j) {
            wchar_t c = arg[j];
            if (c == L'\\') ++slash;
            else {
                if (c == L'"') {
                    for (size_t k = 0; k <= slash && b->status == XR_PROC_OK; ++k) proc_emit(b, out, &pos, L'\\');
                }
                slash = 0;
            }
            proc_emit(b, out, &pos, c);
        }
        if (quotes) {
            for (size_t k = 0; k < slash && b->status == XR_PROC_OK; ++k) proc_emit(b, out, &pos, L'\\');
            proc_emit(b, out, &pos, L'"');
        }
        proc_free(b, arg);
    }
    proc_emit(b, out, &pos, 0);
    if (b->status != XR_PROC_OK) { proc_free(b, out); return NULL; }
    return out;
}
static bool proc_wlength(ProcBuild *b, const wchar_t *s, size_t *n) {
    *n = 0;
    for (;;) {
        if (!proc_work(b, sizeof(wchar_t))) return false;
        if (!s[*n]) return true;
        ++*n;
    }
}
static bool proc_exact_program(ProcBuild *b, const wchar_t *path, bool *exact) {
    *exact = false;
    bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    bool unc = (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/');
    if (!drive && !unc) return true;
    /* A complete filename needs neither PATH lookup nor extension expansion.
     * Single-root and drive-relative names still use runtime resolution. */
    for (size_t i = 0;; ++i) {
        if (!proc_work(b, sizeof(wchar_t))) return false;
        if (!path[i]) return true;
        if (path[i] == L'\\' || path[i] == L'/') *exact = false;
        else if (path[i] == L'.') *exact = true;
    }
}
static wchar_t *proc_environment(ProcBuild *b, const XrProcSpawnOptions *o) {
    LPWCH inherited = NULL; wchar_t **entries = NULL, *block = NULL;
    size_t count = 0, cap = o->env_count, bytes = 2 * sizeof(wchar_t);
    if (!o->complete_environment) {
        if (!proc_work(b, 1)) goto done;
        inherited = GetEnvironmentStringsW();
        if (!inherited) { b->status = xr_proc_last_error(); goto done; }
        for (const wchar_t *p = inherited; *p;) {
            size_t n; if (!proc_wlength(b, p, &n)) goto done;
            ++cap; p += n + 1;
        }
    }
    if (cap > SIZE_MAX / sizeof(*entries)) { b->status = XR_PROC_BUDGET; goto done; }
    if (cap) {
        entries = proc_alloc(b, cap * sizeof(*entries)); if (!entries) goto done;
    }
    if (inherited) for (const wchar_t *p = inherited; *p;) {
        size_t n; if (!proc_wlength(b, p, &n)) goto done;
        wchar_t *copy = proc_alloc(b, (n + 1) * sizeof(wchar_t)); if (!copy) goto done;
        entries[count++] = copy;
        if (!proc_work(b, (n + 1) * sizeof(wchar_t))) goto done;
        memcpy(copy, p, (n + 1) * sizeof(wchar_t)); p += n + 1;
    }
    for (size_t i = 0; i < o->env_count; ++i) {
        wchar_t *key = proc_wide(b, o->env_keys[i]), *value = NULL, *pair = NULL;
        if (!key) goto done;
        value = proc_wide(b, o->env_values[i]);
        if (!value) { proc_free(b, key); goto done; }
        size_t k = 0, v = 0;
        if (!proc_wlength(b, key, &k) || !proc_wlength(b, value, &v)) goto pair_done;
        if (!proc_work(b, k * sizeof(wchar_t))) goto pair_done;
        bool drive_key = k == 3 && key[0] == L'=' && ((key[1] >= L'A' && key[1] <= L'Z') || (key[1] >= L'a' && key[1] <= L'z')) && key[2] == L':';
        if (k >= INT_MAX) { b->status = XR_PROC_BUDGET; goto pair_done; }
        if (!k || (!drive_key && wcschr(key, L'='))) { b->status = XR_PROC_INVALID_ARGUMENT; goto pair_done; }
        if (k > SIZE_MAX / sizeof(wchar_t) - v - 2) { b->status = XR_PROC_BUDGET; goto pair_done; }
        pair = proc_alloc(b, (k + v + 2) * sizeof(wchar_t)); if (!pair) goto pair_done;
        if (!proc_work(b, (k + v + 2) * sizeof(wchar_t))) goto pair_done;
        memcpy(pair, key, k * sizeof(wchar_t)); pair[k] = L'=';
        memcpy(pair + k + 1, value, (v + 1) * sizeof(wchar_t));
        size_t slot = count;
        for (size_t j = 0; j < count; ++j) {
            size_t n;
            if (!proc_wlength(b, entries[j], &n) || !proc_work(b, (k + 1) * sizeof(wchar_t))) break;
            if (n > k && entries[j][k] == L'=') {
                int order = CompareStringOrdinal(entries[j], (int)k, key, (int)k, TRUE);
                if (!order) { b->status = xr_proc_last_error(); break; }
                if (order == CSTR_EQUAL) { slot = j; break; }
            }
        }
        if (b->status == XR_PROC_OK) {
            if (slot < count) proc_free(b, entries[slot]); else ++count;
            entries[slot] = pair; pair = NULL;
        }
    pair_done:
        proc_free(b, pair); proc_free(b, key); proc_free(b, value);
        if (b->status != XR_PROC_OK) goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t n; if (!proc_wlength(b, entries[i], &n)) goto done;
        if (n + 1 > (SIZE_MAX - bytes) / sizeof(wchar_t)) { b->status = XR_PROC_BUDGET; goto done; }
        bytes += (n + 1) * sizeof(wchar_t);
        for (size_t j = i; j > 0; --j) {
            size_t previous;
            if (!proc_wlength(b, entries[j - 1], &previous) || !proc_work(b, (n + previous) * sizeof(wchar_t))) goto done;
            if (n >= INT_MAX || previous >= INT_MAX) { b->status = XR_PROC_BUDGET; goto done; }
            int order = CompareStringOrdinal(entries[j - 1], (int)previous, entries[j], (int)n, TRUE);
            if (!order) { b->status = xr_proc_last_error(); goto done; }
            if (order != CSTR_GREATER_THAN) break;
            if (!proc_work(b, 3 * sizeof(wchar_t *))) goto done;
            wchar_t *swap = entries[j]; entries[j] = entries[j - 1]; entries[j - 1] = swap;
        }
    }
    block = proc_alloc(b, bytes); if (!block) goto done;
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t n; if (!proc_wlength(b, entries[i], &n) || !proc_work(b, (n + 1) * sizeof(wchar_t))) goto done;
        memcpy(block + at, entries[i], (n + 1) * sizeof(wchar_t)); at += n + 1;
    }
    if (!proc_work(b, 2 * sizeof(wchar_t))) goto done;
    block[at] = 0; block[at + 1] = 0;
 done:
    for (size_t i = 0; i < count; ++i) proc_free(b, entries[i]);
    proc_free(b, entries);
    if (inherited) FreeEnvironmentStringsW(inherited);
    if (b->status != XR_PROC_OK) { proc_free(b, block); block = NULL; }
    return block;
}

XR_FUNC XrOsProcStatus xr_proc_spawn(const char *prog, const char *const argv[],
    const XrProcSpawnOptions *o, XrProcId *output) {
    if (!prog || !*prog || !argv || !argv[0] || !o || !output || *output != XR_PROC_INVALID ||
        !o->memory.alloc || !o->memory.free || !o->memory.work ||
        (o->env_count && (!o->env_keys || !o->env_values)) || (o->detached && o->new_process_group)) return XR_PROC_INVALID_ARGUMENT;
    ProcBuild b = {o->memory, XR_PROC_OK};
    wchar_t *command = proc_command(&b, argv), *program = NULL, *cwd = NULL, *environment = NULL;
    HANDLE job = NULL, redirected[3] = {NULL, NULL, NULL};
    STARTUPINFOEXW si = {0}; PROCESS_INFORMATION pi = {0}; bool attributes_ready = false, started = false;
    si.StartupInfo.cb = sizeof(si); int slot = -1;
    if (!command) goto done;
    program = proc_wide(&b, prog); if (!program) goto done;
    bool exact_program = false;
    if (!o->complete_environment && !proc_exact_program(&b, program, &exact_program)) goto done;
    if (!o->complete_environment && !exact_program) {
        if (!proc_work(&b, 1)) goto done;
        DWORD needed = SearchPathW(NULL, program, L".exe", 0, NULL, NULL);
        if (!needed) { b.status = xr_proc_last_error(); goto done; }
        wchar_t *resolved = proc_alloc(&b, (size_t)needed * sizeof(wchar_t));
        if (!resolved) goto done;
        if (!proc_work(&b, 1 + (uint64_t)needed * sizeof(wchar_t))) { proc_free(&b, resolved); goto done; }
        DWORD length = SearchPathW(NULL, program, L".exe", needed, resolved, NULL);
        if (!length || length >= needed) {
            b.status = !length ? xr_proc_last_error() : XR_PROC_IO; proc_free(&b, resolved); goto done;
        }
        proc_free(&b, program); program = resolved;
    }
    if (o->cwd && *o->cwd) { cwd = proc_wide(&b, o->cwd); if (!cwd) goto done; }
    environment = proc_environment(&b, o); if (!environment) goto done;
    if (!o->detached) {
        AcquireSRWLockExclusive(&g_live_lock);
        for (int i = 0; i < XR_PROC_MAX_LIVE; ++i) if (!g_live[i].pid) { slot = i; g_live[i].pid = UINT32_MAX; break; }
        ReleaseSRWLockExclusive(&g_live_lock);
        if (slot < 0) { b.status = XR_PROC_BUDGET; goto done; }
    }
    if (o->new_process_group) {
        if (!proc_work(&b, 1)) goto done;
        job = CreateJobObjectW(NULL, NULL); if (!job) { b.status = xr_proc_last_error(); goto done; }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!proc_work(&b, 1)) goto done;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { b.status = xr_proc_last_error(); goto done; }
    }
    {
        HANDLE originals[3] = {o->has_stdin ? (HANDLE)(intptr_t)o->stdin_read : GetStdHandle(STD_INPUT_HANDLE),
            o->has_stdout ? (HANDLE)(intptr_t)o->stdout_write : GetStdHandle(STD_OUTPUT_HANDLE),
            o->has_stderr ? (HANDLE)(intptr_t)o->stderr_write : GetStdHandle(STD_ERROR_HANDLE)};
        for (unsigned i = 0; i < 3; ++i) {
            if (!proc_work(&b, 1)) goto done;
            if (!originals[i] || originals[i] == INVALID_HANDLE_VALUE) {
                SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
                redirected[i] = CreateFileW(L"NUL", i ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
                if (redirected[i] == INVALID_HANDLE_VALUE) { redirected[i] = NULL; b.status = xr_proc_last_error(); goto done; }
            } else if (!DuplicateHandle(GetCurrentProcess(), originals[i], GetCurrentProcess(), &redirected[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) { b.status = xr_proc_last_error(); goto done; }
        }
        SIZE_T size = 0;
        if (!proc_work(&b, 1)) goto done;
        (void)InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        if (!size) { b.status = xr_proc_last_error(); goto done; }
        si.lpAttributeList = proc_alloc(&b, size); if (!si.lpAttributeList) goto done;
        if (!proc_work(&b, 1)) goto done;
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) { b.status = xr_proc_last_error(); goto done; }
        attributes_ready = true;
        if (!proc_work(&b, 1)) goto done;
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, redirected, sizeof(redirected), NULL, NULL)) { b.status = xr_proc_last_error(); goto done; }
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = redirected[0]; si.StartupInfo.hStdOutput = redirected[1]; si.StartupInfo.hStdError = redirected[2];
    }
    if (!proc_work(&b, 1)) goto done;
    if (!CreateProcessW(program, command, NULL, NULL, attributes_ready, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
        (o->new_process_group ? CREATE_NO_WINDOW : 0) |
        (attributes_ready ? EXTENDED_STARTUPINFO_PRESENT : 0) | ((o->detached || o->new_process_group) ? CREATE_NEW_PROCESS_GROUP : 0), environment, cwd, &si.StartupInfo, &pi)) { b.status = xr_proc_last_error(); goto done; }
    started = true;
    if (job) {
        if (!proc_work(&b, 1)) goto done;
        if (!AssignProcessToJobObject(job, pi.hProcess)) { b.status = xr_proc_last_error(); goto done; }
    }
    if (!proc_work(&b, 1)) goto done;
    if (ResumeThread(pi.hThread) == (DWORD)-1) { b.status = xr_proc_last_error(); goto done; }
    if (slot >= 0) {
        AcquireSRWLockExclusive(&g_live_lock);
        g_live[slot] = (ProcLive){pi.dwProcessId, pi.hProcess, job, false};
        ReleaseSRWLockExclusive(&g_live_lock); pi.hProcess = NULL; job = NULL;
    }
    *output = (XrProcId)pi.dwProcessId;
 done:
    if (b.status != XR_PROC_OK && started) {
        if (job) (void)TerminateJobObject(job, XR_PROC_KILLED_EXIT_CODE);
        (void)TerminateProcess(pi.hProcess, XR_PROC_KILLED_EXIT_CODE);
        (void)WaitForSingleObject(pi.hProcess, INFINITE);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (b.status != XR_PROC_OK && slot >= 0) {
        AcquireSRWLockExclusive(&g_live_lock); memset(&g_live[slot], 0, sizeof(g_live[slot])); ReleaseSRWLockExclusive(&g_live_lock);
    }
    if (attributes_ready) DeleteProcThreadAttributeList(si.lpAttributeList);
    proc_free(&b, si.lpAttributeList);
    for (unsigned i = 0; i < 3; ++i) if (redirected[i]) CloseHandle(redirected[i]);
    proc_free(&b, environment); proc_free(&b, cwd); proc_free(&b, program); proc_free(&b, command);
    return b.status;
}
static ProcLive *proc_find(XrProcId pid) {
    if (pid <= 0 || (uint64_t)pid > UINT32_MAX) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    for (int i = 0; i < XR_PROC_MAX_LIVE; ++i) if (g_live[i].pid == (DWORD)pid && g_live[i].process) return &g_live[i];
    SetLastError(ERROR_INVALID_HANDLE); return NULL;
}
static XrProcWaitResult proc_wait(XrProcId pid, DWORD timeout, int *exit_code) {
    HANDLE lease = NULL, original = NULL;
    AcquireSRWLockExclusive(&g_live_lock); ProcLive *p = proc_find(pid);
    if (p && !p->waited) {
        original = p->process;
        if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &lease,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            DWORD error = GetLastError(); ReleaseSRWLockExclusive(&g_live_lock);
            SetLastError(error); return XR_PROC_WAIT_ERROR;
        }
    }
    ReleaseSRWLockExclusive(&g_live_lock);
    if (!lease) { SetLastError(ERROR_INVALID_HANDLE); return XR_PROC_WAIT_ERROR; }
    XrProcWaitResult result = XR_PROC_WAIT_ERROR; DWORD error = ERROR_SUCCESS;
    DWORD wait = WaitForSingleObject(lease, timeout), code = 0;
    if (wait == WAIT_TIMEOUT) result = XR_PROC_WAIT_RUNNING;
    else if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(lease, &code)) error = GetLastError();
    else {
        AcquireSRWLockExclusive(&g_live_lock); p = proc_find(pid);
        if (!p || p->process != original || p->waited) error = ERROR_INVALID_HANDLE;
        else {
            if (exit_code) *exit_code = code == XR_PROC_KILLED_EXIT_CODE ? -1 : (int)code;
            p->waited = true; result = XR_PROC_WAIT_EXITED;
            if (!p->job) { CloseHandle(p->process); memset(p, 0, sizeof(*p)); }
        }
        ReleaseSRWLockExclusive(&g_live_lock);
    }
    CloseHandle(lease);
    if (result == XR_PROC_WAIT_ERROR) SetLastError(error);
    return result;
}
XR_FUNC int xr_proc_wait(XrProcId pid, int *exit_code) { return proc_wait(pid, INFINITE, exit_code) == XR_PROC_WAIT_EXITED ? 0 : -1; }
XR_FUNC XrProcWaitResult xr_proc_try_wait(XrProcId pid, int *exit_code) { return proc_wait(pid, 0, exit_code); }
XR_FUNC int xr_proc_kill(XrProcId pid, int signal) {
    if (pid <= 0 || (uint64_t)pid > UINT32_MAX || signal <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!process) return -1;
    BOOL ok = TerminateProcess(process, XR_PROC_KILLED_EXIT_CODE); DWORD error = GetLastError();
    CloseHandle(process); if (!ok) SetLastError(error); return ok ? 0 : -1;
}
XR_FUNC int xr_proc_kill_tree(XrProcId pid, int signal) {
    AcquireSRWLockExclusive(&g_live_lock); ProcLive *p = proc_find(pid);
    bool ok = p && p->job && signal > 0 && TerminateJobObject(p->job, XR_PROC_KILLED_EXIT_CODE);
    ReleaseSRWLockExclusive(&g_live_lock); return ok ? 0 : -1;
}
XR_FUNC int xr_proc_close(XrProcId pid) {
    AcquireSRWLockExclusive(&g_live_lock); ProcLive *p = proc_find(pid); ProcLive owner = {0};
    if (p && p->job) { owner = *p; memset(p, 0, sizeof(*p)); }
    ReleaseSRWLockExclusive(&g_live_lock); if (!owner.process) return -1;
    bool ok = true;
    if (owner.job) {
        if (!TerminateJobObject(owner.job, XR_PROC_KILLED_EXIT_CODE)) ok = false;
        /* Closing the kill-on-close job still stops descendants after a failed explicit termination. */
        if (!CloseHandle(owner.job)) ok = false;
    }
    if (!owner.waited) {
        if (!TerminateProcess(owner.process, XR_PROC_KILLED_EXIT_CODE) && WaitForSingleObject(owner.process, 0) != WAIT_OBJECT_0) ok = false;
        if (WaitForSingleObject(owner.process, INFINITE) != WAIT_OBJECT_0) ok = false;
    }
    if (!CloseHandle(owner.process)) ok = false;
    return ok ? 0 : -1;
}

int64_t xr_proc_self_pid(void) {
    return (int64_t) GetCurrentProcessId();
}

int xr_proc_self_exe_path(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }
    wchar_t wide[32768];
    DWORD n = GetModuleFileNameW(NULL, wide, (DWORD) (sizeof(wide) / sizeof(wide[0])));
    /* n == 0 means failure; n == capacity means truncation. */
    if (n == 0 || (size_t) n >= sizeof(wide) / sizeof(wide[0]) ||
        !xr_win_utf16_to_utf8(wide, (size_t) n, buf, size)) {
        return -1;
    }
    return 0;
}

bool xr_proc_debugger_attached(void) {
    return IsDebuggerPresent() ? true : false;
}
