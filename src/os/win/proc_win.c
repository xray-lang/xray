/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_win.c - Windows implementation of os_proc.h.
 *
 * The shim hands callers an int64_t XrProcId. We can't return a
 * raw HANDLE from CreateProcess because it's pointer-sized and not
 * a process identifier. Instead we maintain a tiny intrusive table
 * of (pid, handle) pairs so xr_proc_wait can look the handle back
 * up. The table size is bounded by XR_PROC_MAX_LIVE; once a child
 * is waited-on the slot is freed.
 */

#include "../os_proc.h"
#include "../../shared/xr_win_utf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define XR_PROC_MAX_LIVE 64
#define XR_PROC_KILLED_EXIT_CODE ((DWORD) 0xE0000001u)

static struct {
    DWORD pid;
    HANDLE handle;
} g_live[XR_PROC_MAX_LIVE];
static SRWLOCK g_live_lock = SRWLOCK_INIT;

static void live_record(DWORD pid, HANDLE h) {
    AcquireSRWLockExclusive(&g_live_lock);
    for (int i = 0; i < XR_PROC_MAX_LIVE; i++) {
        if (g_live[i].handle == NULL) {
            g_live[i].pid = pid;
            g_live[i].handle = h;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_live_lock);
}

static HANDLE live_pop(DWORD pid) {
    HANDLE h = NULL;
    AcquireSRWLockExclusive(&g_live_lock);
    for (int i = 0; i < XR_PROC_MAX_LIVE; i++) {
        if (g_live[i].handle != NULL && g_live[i].pid == pid) {
            h = g_live[i].handle;
            g_live[i].handle = NULL;
            g_live[i].pid = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_live_lock);
    return h;
}

// Write one quoted argument body into `buf`, escaping per the
// Microsoft CommandLineToArgvW contract: a run of backslashes is only
// doubled when it precedes a literal `"` or the closing quote; ordinary
// backslashes are emitted verbatim. The caller writes the surrounding quotes.
static void append_escaped_arg(wchar_t *buf, size_t *pos, const wchar_t *arg) {
    size_t backslashes = 0;
    for (const wchar_t *p = arg; *p; p++) {
        if (*p == L'\\') {
            backslashes++;
            buf[(*pos)++] = L'\\';
        } else if (*p == L'"') {
            for (size_t k = 0; k < backslashes; k++)
                buf[(*pos)++] = L'\\';
            buf[(*pos)++] = L'\\';
            buf[(*pos)++] = L'"';
            backslashes = 0;
        } else {
            backslashes = 0;
            buf[(*pos)++] = *p;
        }
    }
    for (size_t k = 0; k < backslashes; k++)
        buf[(*pos)++] = L'\\';
}

static bool command_arg_needs_quotes(const wchar_t *arg) {
    return arg[0] == L'\0' || wcspbrk(arg, L" \t\"") != NULL;
}

static void append_command_arg(wchar_t *buf, size_t *pos, const wchar_t *arg) {
    if (!command_arg_needs_quotes(arg)) {
        size_t len = wcslen(arg);
        memcpy(buf + *pos, arg, len * sizeof(wchar_t));
        *pos += len;
        return;
    }
    buf[(*pos)++] = L'"';
    append_escaped_arg(buf, pos, arg);
    buf[(*pos)++] = L'"';
}

static wchar_t *proc_utf8_dup(const char *input) {
    if (!input)
        return NULL;
    size_t len = strlen(input);
    int required = xr_win_utf8_to_utf16_required(input, len);
    if (required == 0 || (size_t) required > SIZE_MAX / sizeof(wchar_t))
        return NULL;
    wchar_t *output = (wchar_t *) malloc((size_t) required * sizeof(wchar_t));
    if (!output || !xr_win_utf8_to_utf16(input, len, output, (size_t) required)) {
        free(output);  // xr:allow-raw-alloc
        return NULL;
    }
    return output;
}

// Quote-and-join argv into a single command line. Arguments are quoted only
// when the Windows argv contract requires it. Besides producing the same argv
// for ordinary programs, this matters for cmd.exe: quoting every token turns
// builtins and control operators into literal text.
static wchar_t *build_command_line(const char *prog, const char *const argv[]) {
    size_t cap = strlen(prog) * 2 + 3;
    for (int i = 0; argv[i] != NULL; i++) {
        cap += strlen(argv[i]) * 2 + 3;
    }
    if (cap > SIZE_MAX / sizeof(wchar_t) - 1)
        return NULL;
    wchar_t *buf = (wchar_t *) malloc((cap + 1) * sizeof(wchar_t));  // xr:allow-raw-alloc
    if (!buf) {
        return NULL;
    }
    size_t pos = 0;
    if (argv[0] == NULL || strcmp(argv[0], prog) != 0) {
        wchar_t *wide = proc_utf8_dup(prog);
        if (!wide) {
            free(buf);  // xr:allow-raw-alloc
            return NULL;
        }
        append_command_arg(buf, &pos, wide);
        free(wide);  // xr:allow-raw-alloc
    }
    for (int i = 0; argv[i] != NULL; i++) {
        if (pos > 0) {
            buf[pos++] = L' ';
        }
        wchar_t *wide = proc_utf8_dup(argv[i]);
        if (!wide) {
            free(buf);  // xr:allow-raw-alloc
            return NULL;
        }
        append_command_arg(buf, &pos, wide);
        free(wide);  // xr:allow-raw-alloc
    }
    buf[pos] = L'\0';
    return buf;
}

static bool proc_env_key_valid(const char *key) {
    if (!key || key[0] == '\0')
        return false;
    return strchr(key, '=') == NULL;
}

static bool proc_spawn_options_valid(const XrProcSpawnOptions *options) {
    if (!options || options->env_count == 0)
        return true;
    if (!options->env_keys || !options->env_values)
        return false;
    for (size_t i = 0; i < options->env_count; i++) {
        if (!proc_env_key_valid(options->env_keys[i]) || !options->env_values[i])
            return false;
    }
    return true;
}

static wchar_t *proc_wcsdup(const wchar_t *s) {
    size_t len = wcslen(s);
    wchar_t *out = (wchar_t *) malloc((len + 1) * sizeof(wchar_t));  // xr:allow-raw-alloc
    if (!out)
        return NULL;
    memcpy(out, s, (len + 1) * sizeof(wchar_t));
    return out;
}

static bool proc_env_entry_matches_key(const wchar_t *entry, const wchar_t *key) {
    const wchar_t *eq = entry ? wcschr(entry, L'=') : NULL;
    if (!eq || eq == entry || !key)
        return false;
    size_t name_len = (size_t) (eq - entry);
    return wcslen(key) == name_len && _wcsnicmp(entry, key, name_len) == 0;
}

static bool proc_env_key_is_overridden(const XrProcSpawnOptions *options, const wchar_t *entry) {
    for (size_t i = 0; i < options->env_count; i++) {
        wchar_t *key = proc_utf8_dup(options->env_keys[i]);
        if (!key)
            return false;
        bool match = proc_env_entry_matches_key(entry, key);
        free(key);  // xr:allow-raw-alloc
        if (match)
            return true;
    }
    return false;
}

static int proc_env_entry_cmp(const void *a, const void *b) {
    const wchar_t *ea = *(const wchar_t *const *) a;
    const wchar_t *eb = *(const wchar_t *const *) b;
    return _wcsicmp(ea, eb);
}

typedef struct ProcEnvEntries {
    wchar_t **items;
    size_t count;
    size_t cap;
} ProcEnvEntries;

static void proc_env_entries_free(ProcEnvEntries *entries) {
    if (!entries)
        return;
    for (size_t i = 0; i < entries->count; i++)
        free(entries->items[i]);  // xr:allow-raw-alloc
    free(entries->items);         // xr:allow-raw-alloc
    entries->items = NULL;
    entries->count = 0;
    entries->cap = 0;
}

static bool proc_env_entries_push(ProcEnvEntries *entries, wchar_t *item) {
    if (entries->count == entries->cap) {
        size_t next_cap = entries->cap ? entries->cap * 2 : 32;
        wchar_t **next =
            (wchar_t **) realloc(entries->items, next_cap * sizeof(wchar_t *));
        if (!next)
            return false;
        entries->items = next;
        entries->cap = next_cap;
    }
    entries->items[entries->count++] = item;
    return true;
}

static wchar_t *proc_env_pair_new(const char *key, const char *value) {
    wchar_t *wide_key = proc_utf8_dup(key);
    wchar_t *wide_value = proc_utf8_dup(value);
    if (!wide_key || !wide_value) {
        free(wide_key);    // xr:allow-raw-alloc
        free(wide_value);  // xr:allow-raw-alloc
        return NULL;
    }
    size_t key_len = wcslen(wide_key);
    size_t value_len = wcslen(wide_value);
    if (key_len > SIZE_MAX - value_len - 2)
        goto fail;
    wchar_t *out =
        (wchar_t *) malloc((key_len + value_len + 2) * sizeof(wchar_t));  // xr:allow-raw-alloc
    if (!out)
        goto fail;
    memcpy(out, wide_key, key_len * sizeof(wchar_t));
    out[key_len] = L'=';
    memcpy(out + key_len + 1, wide_value, (value_len + 1) * sizeof(wchar_t));
    free(wide_key);    // xr:allow-raw-alloc
    free(wide_value);  // xr:allow-raw-alloc
    return out;

fail:
    free(wide_key);    // xr:allow-raw-alloc
    free(wide_value);  // xr:allow-raw-alloc
    return NULL;
}

static wchar_t *proc_env_block_build(const XrProcSpawnOptions *options) {
    if (!options || options->env_count == 0)
        return NULL;

    ProcEnvEntries entries = {0};
    LPWCH current = GetEnvironmentStringsW();
    if (!current)
        return NULL;
    for (const wchar_t *p = current; *p; p += wcslen(p) + 1) {
        if (proc_env_key_is_overridden(options, p))
            continue;
        wchar_t *copy = proc_wcsdup(p);
        if (!copy || !proc_env_entries_push(&entries, copy)) {
            free(copy);  // xr:allow-raw-alloc
            FreeEnvironmentStringsW(current);
            proc_env_entries_free(&entries);
            return NULL;
        }
    }
    FreeEnvironmentStringsW(current);

    for (size_t i = 0; i < options->env_count; i++) {
        wchar_t *pair = proc_env_pair_new(options->env_keys[i], options->env_values[i]);
        if (!pair || !proc_env_entries_push(&entries, pair)) {
            free(pair);  // xr:allow-raw-alloc
            proc_env_entries_free(&entries);
            return NULL;
        }
    }

    qsort(entries.items, entries.count, sizeof(wchar_t *), proc_env_entry_cmp);

    size_t units = 1;
    for (size_t i = 0; i < entries.count; i++) {
        size_t len = wcslen(entries.items[i]);
        if (units > SIZE_MAX - len - 1) {
            proc_env_entries_free(&entries);
            return NULL;
        }
        units += len + 1;
    }

    if (units > SIZE_MAX / sizeof(wchar_t)) {
        proc_env_entries_free(&entries);
        return NULL;
    }
    wchar_t *block = (wchar_t *) malloc(units * sizeof(wchar_t));  // xr:allow-raw-alloc
    if (!block) {
        proc_env_entries_free(&entries);
        return NULL;
    }
    wchar_t *dst = block;
    for (size_t i = 0; i < entries.count; i++) {
        size_t len = wcslen(entries.items[i]) + 1;
        memcpy(dst, entries.items[i], len * sizeof(wchar_t));
        dst += len;
    }
    *dst = L'\0';
    proc_env_entries_free(&entries);
    return block;
}

typedef struct ProcStdioDup {
    HANDLE in;
    HANDLE out;
    HANDLE err;
} ProcStdioDup;

static void proc_stdio_dup_close(ProcStdioDup *dup) {
    if (!dup)
        return;
    if (dup->in)
        CloseHandle(dup->in);
    if (dup->out)
        CloseHandle(dup->out);
    if (dup->err)
        CloseHandle(dup->err);
    dup->in = NULL;
    dup->out = NULL;
    dup->err = NULL;
}

static bool proc_duplicate_inheritable(HANDLE src, HANDLE *out) {
    if (!src || src == INVALID_HANDLE_VALUE || !out)
        return false;
    HANDLE current = GetCurrentProcess();
    return DuplicateHandle(current, src, current, out, 0, TRUE, DUPLICATE_SAME_ACCESS) ? true
                                                                                       : false;
}

static bool proc_stdio_prepare(const XrProcSpawnOptions *options, STARTUPINFOW *si,
                               ProcStdioDup *dup) {
    if (!options || (!options->has_stdin && !options->has_stdout && !options->has_stderr))
        return true;

    memset(dup, 0, sizeof(*dup));
    si->dwFlags |= STARTF_USESTDHANDLES;
    si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si->hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (options->has_stdin) {
        if (!proc_duplicate_inheritable((HANDLE) (intptr_t) options->stdin_read, &dup->in))
            return false;
        si->hStdInput = dup->in;
    }
    if (options->has_stdout) {
        if (!proc_duplicate_inheritable((HANDLE) (intptr_t) options->stdout_write, &dup->out))
            return false;
        si->hStdOutput = dup->out;
    }
    if (options->has_stderr) {
        if (!proc_duplicate_inheritable((HANDLE) (intptr_t) options->stderr_write, &dup->err))
            return false;
        si->hStdError = dup->err;
    }
    return true;
}

XrProcId xr_proc_spawn_ex(const char *prog, const char *const argv[],
                          const XrProcSpawnOptions *options) {
    if (prog == NULL || argv == NULL || !proc_spawn_options_valid(options)) {
        return XR_PROC_INVALID;
    }
    wchar_t *cmdline = build_command_line(prog, argv);
    if (!cmdline) {
        return XR_PROC_INVALID;
    }
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ProcStdioDup stdio_dup = {0};
    if (!proc_stdio_prepare(options, &si, &stdio_dup)) {
        proc_stdio_dup_close(&stdio_dup);
        free(cmdline);  // xr:allow-raw-alloc
        return XR_PROC_INVALID;
    }
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    wchar_t *cwd = NULL;
    if (options && options->cwd && options->cwd[0] != '\0') {
        cwd = proc_utf8_dup(options->cwd);
        if (!cwd) {
            proc_stdio_dup_close(&stdio_dup);
            free(cmdline);  // xr:allow-raw-alloc
            return XR_PROC_INVALID;
        }
    }
    wchar_t *env_block = proc_env_block_build(options);
    if (options && options->env_count > 0 && !env_block) {
        proc_stdio_dup_close(&stdio_dup);
        free(cwd);      // xr:allow-raw-alloc
        free(cmdline);  // xr:allow-raw-alloc
        return XR_PROC_INVALID;
    }
    DWORD create_flags = (options && (options->detached || options->new_process_group))
                             ? CREATE_NEW_PROCESS_GROUP
                             : 0;
    if (env_block)
        create_flags |= CREATE_UNICODE_ENVIRONMENT;
    BOOL ok =
        CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, create_flags, env_block, cwd, &si, &pi);
    proc_stdio_dup_close(&stdio_dup);
    free(env_block);  // xr:allow-raw-alloc
    free(cwd);        // xr:allow-raw-alloc
    free(cmdline);    // xr:allow-raw-alloc
    if (!ok) {
        return XR_PROC_INVALID;
    }
    CloseHandle(pi.hThread);
    if (options && options->detached) {
        CloseHandle(pi.hProcess);
        return (XrProcId) pi.dwProcessId;
    }
    live_record(pi.dwProcessId, pi.hProcess);
    return (XrProcId) pi.dwProcessId;
}

XrProcId xr_proc_spawn(const char *prog, const char *const argv[]) {
    return xr_proc_spawn_ex(prog, argv, NULL);
}

int xr_proc_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    HANDLE h = live_pop((DWORD) pid);
    if (h == NULL) {
        // Fall back to OpenProcess so callers from a different
        // path still get a meaningful answer.
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD) pid);
        if (h == NULL) {
            if (exit_code) {
                *exit_code = -1;
            }
            return -1;
        }
    }
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!ok) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    if (exit_code) {
        *exit_code = code == XR_PROC_KILLED_EXIT_CODE ? -1 : (int) code;
    }
    return 0;
}

static void decode_exit_code(DWORD code, int *exit_code) {
    if (exit_code) {
        *exit_code = code == XR_PROC_KILLED_EXIT_CODE ? -1 : (int) code;
    }
}

XrProcWaitResult xr_proc_try_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return XR_PROC_WAIT_ERROR;
    }

    HANDLE h = NULL;
    AcquireSRWLockExclusive(&g_live_lock);
    for (int i = 0; i < XR_PROC_MAX_LIVE; i++) {
        if (g_live[i].handle != NULL && g_live[i].pid == (DWORD) pid) {
            DWORD wait_result = WaitForSingleObject(g_live[i].handle, 0);
            if (wait_result == WAIT_TIMEOUT) {
                ReleaseSRWLockExclusive(&g_live_lock);
                return XR_PROC_WAIT_RUNNING;
            }
            if (wait_result == WAIT_OBJECT_0) {
                h = g_live[i].handle;
                g_live[i].handle = NULL;
                g_live[i].pid = 0;
                break;
            }
            ReleaseSRWLockExclusive(&g_live_lock);
            if (exit_code) {
                *exit_code = -1;
            }
            return XR_PROC_WAIT_ERROR;
        }
    }
    ReleaseSRWLockExclusive(&g_live_lock);

    if (h == NULL) {
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD) pid);
        if (h == NULL) {
            if (exit_code) {
                *exit_code = -1;
            }
            return XR_PROC_WAIT_ERROR;
        }
        DWORD wait_result = WaitForSingleObject(h, 0);
        if (wait_result == WAIT_TIMEOUT) {
            CloseHandle(h);
            return XR_PROC_WAIT_RUNNING;
        }
        if (wait_result != WAIT_OBJECT_0) {
            CloseHandle(h);
            if (exit_code) {
                *exit_code = -1;
            }
            return XR_PROC_WAIT_ERROR;
        }
    }

    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!ok) {
        if (exit_code) {
            *exit_code = -1;
        }
        return XR_PROC_WAIT_ERROR;
    }
    decode_exit_code(code, exit_code);
    return XR_PROC_WAIT_EXITED;
}

int xr_proc_kill(XrProcId pid, int signal) {
    if (pid <= 0 || signal <= 0) {
        return -1;
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD) pid);
    if (h == NULL) {
        return -1;
    }

    BOOL ok = TerminateProcess(h, XR_PROC_KILLED_EXIT_CODE);
    CloseHandle(h);
    return ok ? 0 : -1;
}

int xr_proc_kill_tree(XrProcId pid, int signal) {
    /* Windows process-tree cleanup is completed with the MSVC provider lane.
     * Keep the API fail-closed to the tracked direct child in the meantime. */
    return xr_proc_kill(pid, signal);
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
