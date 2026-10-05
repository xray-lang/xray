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
#include <windows.h>
#include <string.h>
#include <limits.h>

#define XR_PROC_MAX_LIVE 64
#define XR_PROC_KILLED_EXIT_CODE ((DWORD) 0xE0000001u)
typedef struct ProcDebug {
    XrProcMemory memory;
    XrProcImageObserver observer;
    DWORD thread, root, pids[XR_PROC_MAX_LIVE];
    bool initial[XR_PROC_MAX_LIVE];
    uint32_t live, created;
    bool root_exited, pending, dispatched, pumping;
    XrProcCompletionPolicy completion_policy;
    bool execution_ended;
    DEBUG_EVENT event;
    DWORD disposition;
    XrOsProcStatus status;
} ProcDebug;
typedef struct ProcLive {
    DWORD pid; HANDLE process, job; bool waited;
    DWORD debug_thread;
    ProcDebug *debug;
} ProcLive;
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
        if (k >= INT_MAX) { b->status = XR_PROC_BUDGET; goto pair_done; }
        if (!k) { b->status = XR_PROC_INVALID_ARGUMENT; goto pair_done; }
        for (size_t at = 0; at < k; ++at) {
            if (!proc_work(b, sizeof(wchar_t))) goto pair_done;
            if (key[at] == L'=' && (at || k == 1)) { b->status = XR_PROC_INVALID_ARGUMENT; goto pair_done; }
        }
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


/* Debug callbacks and their borrowed handles are confined to the spawning
 * thread. Cleanup uses the same event dispatcher without invoking callbacks
 * or acquiring further resource permission. */
static XrOsProcStatus proc_debug_charge(ProcDebug *d, uint64_t work) {
    if (d->status == XR_PROC_OK) d->status = d->memory.work(d->memory.context, work);
    return d->status;
}
static XrOsProcStatus proc_debug_dispatch(ProcDebug *d, bool cleanup) {
    if (d->dispatched) return XR_PROC_OK;
    if (!cleanup && proc_debug_charge(d, XR_PROC_MAX_LIVE + 1) != XR_PROC_OK) return d->status;
    DWORD pid = d->event.dwProcessId;
    int slot = -1, empty = -1;
    for (int i = 0; i < XR_PROC_MAX_LIVE; ++i) {
        if (d->pids[i] == pid) slot = i;
        if (!d->pids[i] && empty < 0) empty = i;
    }
    d->dispatched = true;
    d->disposition = DBG_CONTINUE;
    HANDLE image = NULL;
    XrProcImageKind kind = XR_PROC_IMAGE_DLL;
    switch (d->event.dwDebugEventCode) {
    case CREATE_PROCESS_DEBUG_EVENT:
        if (slot >= 0 && !cleanup) return XR_PROC_IO;
        if (!cleanup && d->created == UINT32_MAX) return XR_PROC_BUDGET;
        if (d->created != UINT32_MAX) ++d->created;
        ++d->live;
        if (empty >= 0) { d->pids[empty] = pid; d->initial[empty] = true; }
        else if (!cleanup) return XR_PROC_BUDGET;
        image = d->event.u.CreateProcessInfo.hFile; kind = XR_PROC_IMAGE_EXECUTABLE;
        break;
    case EXIT_PROCESS_DEBUG_EVENT:
        if (pid == d->root) d->root_exited = true;
        if (slot >= 0) { d->pids[slot] = 0; d->initial[slot] = false; }
        else if (!cleanup) return XR_PROC_IO;
        if (d->live) --d->live;
        break;
    case LOAD_DLL_DEBUG_EVENT:
        if (slot < 0 && !cleanup) return XR_PROC_IO;
        image = d->event.u.LoadDll.hFile;
        break;
    case EXCEPTION_DEBUG_EVENT:
        d->disposition = DBG_EXCEPTION_NOT_HANDLED;
        if (slot >= 0 && d->initial[slot] && d->event.u.Exception.dwFirstChance &&
            d->event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
            d->initial[slot] = false; d->disposition = DBG_CONTINUE;
        }
        break;
    default: break;
    }
    if (!cleanup && (d->event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT ||
        d->event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)) {
        if (!image || image == INVALID_HANDLE_VALUE) return XR_PROC_UNSUPPORTED;
        XrProcImageEvent event = {(XrProcId)pid, kind, (intptr_t)image};
        return d->observer.observe(d->observer.context, &event);
    }
    return XR_PROC_OK;
}
static XrOsProcStatus proc_debug_continue(ProcDebug *d, bool cleanup) {
    HANDLE *file = d->event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT ? &d->event.u.CreateProcessInfo.hFile :
        d->event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT ? &d->event.u.LoadDll.hFile : NULL;
    if (file && *file && *file != INVALID_HANDLE_VALUE) {
        if (!cleanup && proc_debug_charge(d, 1) != XR_PROC_OK) return d->status;
        if (!CloseHandle(*file)) return xr_proc_last_error();
        *file = NULL;
    }
    if (!cleanup && proc_debug_charge(d, 1) != XR_PROC_OK) return d->status;
    if (!ContinueDebugEvent(d->event.dwProcessId, d->event.dwThreadId, d->disposition)) return xr_proc_last_error();
    d->pending = d->dispatched = false;
    return XR_PROC_OK;
}
static XrOsProcStatus proc_debug_step(ProcDebug *d, bool cleanup, DWORD timeout, bool *progressed) {
    if (!d->pending) {
        if (!cleanup && proc_debug_charge(d, 1) != XR_PROC_OK) return d->status;
        if (!WaitForDebugEvent(&d->event, timeout)) {
            DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) return XR_PROC_OK;
            SetLastError(error); return xr_proc_last_error();
        }
        d->pending = true;
    }
    XrOsProcStatus status = proc_debug_dispatch(d, cleanup);
    if (status == XR_PROC_OK) status = proc_debug_continue(d, cleanup);
    if (status == XR_PROC_OK && progressed) *progressed = true;
    return status;
}
static bool proc_debug_cleanup(ProcDebug *d, HANDLE process) {
    bool ok = true;
    /* Termination still requires acknowledging outstanding debug exit events.
     * A persistent OS failure must not turn cleanup into an infinite wait. */
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (d->pending || !d->root_exited || d->live) {
        if (proc_debug_step(d, true, 10, NULL) != XR_PROC_OK) { ok = false; Sleep(1); }
        if (GetTickCount64() >= deadline) {
            ok = false;
            if (d->pending) {
                DWORD pending_pid = d->event.dwProcessId;
                (void)proc_debug_continue(d, true);
                /* EXIT dispatch already removed this PID from the live set.
                 * A failed Continue must still detach that pending process. */
                if (d->pending) (void)DebugActiveProcessStop(pending_pid);
            }
            for (unsigned i = 0; i < XR_PROC_MAX_LIVE; ++i)
                if (d->pids[i]) (void)DebugActiveProcessStop(d->pids[i]);
            (void)DebugActiveProcessStop(d->root);
            break;
        }
    }
    if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) ok = false;
    return ok;
}

XR_FUNC XrOsProcStatus xr_proc_spawn(const char *prog, const char *const argv[],
    const XrProcSpawnOptions *o, XrProcId *output) {
    if (!prog || !*prog || !argv || !argv[0] || !o || !output || *output != XR_PROC_INVALID ||
        !o->memory.alloc || !o->memory.free || !o->memory.work ||
        (o->env_count && (!o->env_keys || !o->env_values)) || (o->detached && o->new_process_group) ||
        (o->completion_policy != XR_PROC_COMPLETE_TREE && o->completion_policy != XR_PROC_COMPLETE_ROOT) ||
        (o->completion_policy == XR_PROC_COMPLETE_ROOT && o->image_mode != XR_PROC_IMAGES_WINDOWS_TREE) ||
        (o->image_mode != XR_PROC_IMAGES_NONE && o->image_mode != XR_PROC_IMAGES_WINDOWS_TREE) ||
        ((o->image_mode == XR_PROC_IMAGES_WINDOWS_TREE) != (o->image_observer.observe != NULL)) ||
        (o->image_mode == XR_PROC_IMAGES_WINDOWS_TREE && (!o->new_process_group || o->detached))) return XR_PROC_INVALID_ARGUMENT;
    ProcBuild b = {o->memory, XR_PROC_OK};
    wchar_t *command = NULL, *program = NULL, *cwd = NULL, *environment = NULL;
    HANDLE job = NULL, redirected[3] = {NULL, NULL, NULL};
    STARTUPINFOEXW si = {0}; PROCESS_INFORMATION pi = {0}; bool attributes_ready = false, started = false;
    si.StartupInfo.cb = sizeof(si); int slot = -1;
    ProcDebug *debug = NULL;
    DWORD debug_thread = o->image_mode == XR_PROC_IMAGES_WINDOWS_TREE ? GetCurrentThreadId() : 0;
    if (debug_thread) {
        AcquireSRWLockExclusive(&g_live_lock);
        for (int i = 0; i < XR_PROC_MAX_LIVE; ++i)
            if (g_live[i].debug_thread == debug_thread) b.status = XR_PROC_INVALID_ARGUMENT;
        /* Reserve before calling any allocation/work policy: those callbacks
         * may reenter spawn on this thread before a process exists. */
        if (b.status == XR_PROC_OK) {
            for (int i = 0; i < XR_PROC_MAX_LIVE; ++i) if (!g_live[i].pid) {
                slot = i; g_live[i].pid = UINT32_MAX; g_live[i].debug_thread = debug_thread; break;
            }
            if (slot < 0) b.status = XR_PROC_BUDGET;
        }
        ReleaseSRWLockExclusive(&g_live_lock);
        if (b.status != XR_PROC_OK) goto done;
        debug = proc_alloc(&b, sizeof(*debug)); if (!debug) goto done;
        if (!proc_work(&b, sizeof(*debug) + sizeof(o->memory) + sizeof(o->image_observer) + sizeof(o->completion_policy))) goto done;
        memset(debug, 0, sizeof(*debug)); debug->memory = o->memory;
        debug->observer = o->image_observer; debug->thread = debug_thread;
        debug->completion_policy = o->completion_policy;
    }
    command = proc_command(&b, argv); if (!command) goto done;
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
    if (!o->detached && slot < 0) {
        AcquireSRWLockExclusive(&g_live_lock);
        for (int i = 0; i < XR_PROC_MAX_LIVE; ++i) if (!g_live[i].pid) { slot = i; g_live[i].pid = UINT32_MAX; g_live[i].debug_thread = debug_thread; break; }
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
        (o->new_process_group ? CREATE_NO_WINDOW : 0) | (debug ? DEBUG_PROCESS : 0) |
        (attributes_ready ? EXTENDED_STARTUPINFO_PRESENT : 0) | ((o->detached || o->new_process_group) ? CREATE_NEW_PROCESS_GROUP : 0), environment, cwd, &si.StartupInfo, &pi)) { b.status = xr_proc_last_error(); goto done; }
    started = true; if (debug) debug->root = pi.dwProcessId;
    if (job) {
        if (!proc_work(&b, 1)) goto done;
        if (!AssignProcessToJobObject(job, pi.hProcess)) { b.status = xr_proc_last_error(); goto done; }
    }
    if (!proc_work(&b, 1)) goto done;
    if (ResumeThread(pi.hThread) == (DWORD)-1) { b.status = xr_proc_last_error(); goto done; }
    if (slot >= 0) {
        AcquireSRWLockExclusive(&g_live_lock);
        g_live[slot] = (ProcLive){pi.dwProcessId, pi.hProcess, job, false, debug_thread, debug};
        debug = NULL;
        ReleaseSRWLockExclusive(&g_live_lock); pi.hProcess = NULL; job = NULL;
    }
    *output = (XrProcId)pi.dwProcessId;
 done:
    if (b.status != XR_PROC_OK && started) {
        if (job) (void)TerminateJobObject(job, XR_PROC_KILLED_EXIT_CODE);
        (void)TerminateProcess(pi.hProcess, XR_PROC_KILLED_EXIT_CODE);
        if (debug) (void)proc_debug_cleanup(debug, pi.hProcess);
        else (void)WaitForSingleObject(pi.hProcess, INFINITE);
    }
    proc_free(&b, debug);
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

XR_FUNC XrOsProcStatus xr_proc_pump_images(XrProcId pid, XrProcImagePumpResult *output) {
    if (!output) return XR_PROC_INVALID_ARGUMENT;
    AcquireSRWLockExclusive(&g_live_lock);
    ProcLive *p = proc_find(pid);
    ProcDebug *d = p ? p->debug : NULL;
    HANDLE job = p ? p->job : NULL;
    if (!d || d->thread != GetCurrentThreadId() || d->pumping) {
        ReleaseSRWLockExclusive(&g_live_lock); return XR_PROC_INVALID_ARGUMENT;
    }
    d->pumping = true;
    ReleaseSRWLockExclusive(&g_live_lock);
    XrOsProcStatus status = d->status;
    bool progressed = false;
    if (status == XR_PROC_OK && (!d->root_exited || d->live || d->pending))
        status = proc_debug_step(d, false, 1, &progressed);
    if (status == XR_PROC_OK) status = proc_debug_charge(d, sizeof(d->completion_policy));
    if (status == XR_PROC_OK && d->completion_policy == XR_PROC_COMPLETE_ROOT) {
        status = proc_debug_charge(d, sizeof(d->root_exited) + sizeof(d->pending) + sizeof(d->execution_ended));
        if (status == XR_PROC_OK && d->root_exited && !d->pending && !d->execution_ended) {
            status = proc_debug_charge(d, 1 + sizeof(d->execution_ended));
            if (status == XR_PROC_OK) {
                if (!TerminateJobObject(job, XR_PROC_KILLED_EXIT_CODE)) status = xr_proc_last_error();
                else { d->execution_ended = true; progressed = true; }
            }
        }
    }
    bool complete = d->root_exited && !d->live && !d->pending;
    if (status == XR_PROC_OK && complete) {
        status = proc_debug_charge(d, 1);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
        if (status == XR_PROC_OK) {
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), NULL)) status = xr_proc_last_error();
            else if (accounting.TotalProcesses != d->created) status = XR_PROC_UNSUPPORTED;
            else complete = !accounting.ActiveProcesses;
        }
    }
    d->status = status; d->pumping = false;
    if (status == XR_PROC_OK) *output = (XrProcImagePumpResult){complete, progressed};
    return status;
}
static XrProcWaitResult proc_wait(XrProcId pid, DWORD timeout, int *exit_code) {
    HANDLE lease = NULL, original = NULL;
    AcquireSRWLockExclusive(&g_live_lock); ProcLive *p = proc_find(pid);
    if (p && p->debug && (p->debug->thread != GetCurrentThreadId() || p->debug->pumping || timeout == INFINITE)) {
        ReleaseSRWLockExclusive(&g_live_lock); SetLastError(ERROR_INVALID_PARAMETER); return XR_PROC_WAIT_ERROR;
    }
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
    if (p && p->debug && (p->debug->thread != GetCurrentThreadId() || p->debug->pumping)) {
        ReleaseSRWLockExclusive(&g_live_lock); SetLastError(ERROR_INVALID_PARAMETER); return -1;
    }
    if (p && p->job) { owner = *p; memset(p, 0, sizeof(*p)); }
    ReleaseSRWLockExclusive(&g_live_lock); if (!owner.process) return -1;
    bool ok = true, job_stopped = false;
    if (owner.job) {
        if (!TerminateJobObject(owner.job, XR_PROC_KILLED_EXIT_CODE)) ok = false;
        else job_stopped = true;
        /* Closing the kill-on-close job still stops descendants after a failed explicit termination. */
        if (!CloseHandle(owner.job)) ok = false;
        else job_stopped = true;
    }
    if (owner.debug) {
        /* A killed debuggee cannot signal until its EXIT event is continued.
         * Re-terminating it now may report ACCESS_DENIED despite successful job termination. */
        if (!job_stopped && !TerminateProcess(owner.process, XR_PROC_KILLED_EXIT_CODE) && WaitForSingleObject(owner.process, 0) != WAIT_OBJECT_0) ok = false;
        if (!proc_debug_cleanup(owner.debug, owner.process)) ok = false;
        owner.debug->memory.free(owner.debug->memory.context, owner.debug);
    } else if (!owner.waited) {
        if (!TerminateProcess(owner.process, XR_PROC_KILLED_EXIT_CODE) && WaitForSingleObject(owner.process, 0) != WAIT_OBJECT_0) ok = false;
        if (WaitForSingleObject(owner.process, INFINITE) != WAIT_OBJECT_0) ok = false;
    }
    if (!CloseHandle(owner.process)) ok = false;
    return ok ? 0 : -1;
}
