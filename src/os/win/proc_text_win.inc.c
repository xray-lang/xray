/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_text_win.inc.c - Shared Unicode command and environment preparation
 */
static XrOsProcStatus proc_text_last_error(void) {
    DWORD error=GetLastError();
    if(error==ERROR_NOT_ENOUGH_MEMORY||error==ERROR_OUTOFMEMORY)return XR_PROC_OUT_OF_MEMORY;
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_DIRECTORY)return XR_PROC_UNRESOLVED;
    if(error==ERROR_INVALID_PARAMETER||error==ERROR_NO_UNICODE_TRANSLATION)return XR_PROC_INVALID_ARGUMENT;
    return XR_PROC_IO;
}
typedef struct ProcBuild {
    XrProcMemory memory;
    XrOsProcStatus status;
    void *query_context;
    bool (*current)(void *);
    const wchar_t *inherited;
} ProcBuild;
static bool proc_text_current(ProcBuild *b) {
    if(b->status!=XR_PROC_OK)return false;
    if(b->current&&!b->current(b->query_context)){b->status=XR_PROC_IO;return false;}
    return true;
}
static void *proc_alloc(ProcBuild *b, size_t n) {
    void *p = NULL;
    if (proc_text_current(b)) b->status = b->memory.alloc(b->memory.context, n, &p);
    return p;
}
static void proc_free(ProcBuild *b, void *p) { if (p) b->memory.free(b->memory.context, p); }
static void proc_text_copy(ProcBuild *b,void *output,const void *input,size_t bytes) {
    size_t at=0;
    while(at<bytes&&proc_text_current(b)) {
        size_t chunk=bytes-at;if(chunk>4096)chunk=4096;
        memcpy((uint8_t *)output+at,(const uint8_t *)input+at,chunk);at+=chunk;
    }
}
static bool proc_length(ProcBuild *b, const char *s, size_t *n) {
    if (!s) { b->status = XR_PROC_INVALID_ARGUMENT; return false; }
    *n = 0;
    for (;;) {
        if (((*n)&4095u)==0&&!proc_text_current(b))return false;
        if (!s[*n]) return true;
        if (*n == INT_MAX) { b->status = XR_PROC_BUDGET; return false; }
        ++*n;
    }
}
static wchar_t *proc_wide(ProcBuild *b, const char *s) {
    size_t n;
    if (!proc_length(b, s, &n)) return NULL;
    if (n >= INT_MAX) { b->status = XR_PROC_BUDGET; return NULL; }
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n + 1, NULL, 0);
    if (!units) { b->status = proc_text_last_error(); return NULL; }
    wchar_t *out = proc_alloc(b, (size_t)units * sizeof(wchar_t));
    if (!out) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n + 1, out, units)!=units) {
        if (b->status == XR_PROC_OK) b->status = proc_text_last_error();
        proc_free(b, out); return NULL;
    }
    return out;
}
static void proc_emit(ProcBuild *b, wchar_t *out, size_t *n, wchar_t value) {
    if (((*n)&4095u)!=0||proc_text_current(b)) { if(b->status==XR_PROC_OK)out[(*n)++]=value; }
}
static wchar_t *proc_command(ProcBuild *b, const char *const *argv) {
    size_t cap = 1, count = 0;
    for (; argv[count]; ++count) {
        size_t n;
        if (!proc_length(b, argv[count], &n)) return NULL;
        if (cap > SIZE_MAX - 3 || n > (SIZE_MAX - cap - 3) / 2) { b->status = XR_PROC_OVERFLOW; return NULL; }
        cap += n * 2 + 3;
    }
    if (!count) { b->status = XR_PROC_INVALID_ARGUMENT; return NULL; }
    if (cap > SIZE_MAX / sizeof(wchar_t)) { b->status = XR_PROC_OVERFLOW; return NULL; }
    wchar_t *out = proc_alloc(b, cap * sizeof(wchar_t)); size_t pos = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < count && b->status == XR_PROC_OK; ++i) {
        wchar_t *arg = proc_wide(b, argv[i]); if (!arg) break;
        size_t n = 0; bool quotes = !arg[0];
        while (arg[n]) {
            if ((n&4095u)==0&&!proc_text_current(b))break;
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
        if (((*n)&4095u)==0&&!proc_text_current(b))return false;
        if (!s[*n]) return true;
        if (*n == INT_MAX) { b->status = XR_PROC_BUDGET; return false; }
        ++*n;
    }
}
static bool proc_exact_program(const wchar_t *path) {
    bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    bool unc = (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/');
    if (!drive && !unc) return false;
    /* A complete filename needs neither PATH lookup nor extension expansion. */
    bool exact = false;
    for (size_t i = 0; path[i]; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') exact = false;
        else if (path[i] == L'.') exact = true;
    }
    return exact;
}
static wchar_t *proc_environment(ProcBuild *b, const XrProcSpawnOptions *o) {
    LPWCH inherited = NULL; wchar_t **entries = NULL, *block = NULL;
    size_t count = 0, cap = o->env_count, bytes = 2 * sizeof(wchar_t);
    if (!o->complete_environment) {

        inherited = b->inherited?(LPWCH)b->inherited:GetEnvironmentStringsW();
        if (!inherited) { b->status = proc_text_last_error(); goto done; }
        for (const wchar_t *p = inherited; *p;) {
            size_t n; if (!proc_wlength(b, p, &n)) goto done;
            if (cap == SIZE_MAX) { b->status = XR_PROC_OVERFLOW; goto done; }
            ++cap; p += n + 1;
        }
    }
    if (cap > SIZE_MAX / sizeof(*entries)) { b->status = XR_PROC_OVERFLOW; goto done; }
    if (cap) {
        entries = proc_alloc(b, cap * sizeof(*entries)); if (!entries) goto done;
    }
    if (inherited) for (const wchar_t *p = inherited; *p;) {
        size_t n; if (!proc_wlength(b, p, &n)) goto done;
        wchar_t *copy = proc_alloc(b, (n + 1) * sizeof(wchar_t)); if (!copy) goto done;
        entries[count++] = copy;
        proc_text_copy(b,copy,p,(n+1)*sizeof(wchar_t)); p += n + 1;
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
            if (key[at] == L'=' && (at || k == 1)) { b->status = XR_PROC_INVALID_ARGUMENT; goto pair_done; }
        }
        if (v > SIZE_MAX / sizeof(wchar_t) - 2 || k > SIZE_MAX / sizeof(wchar_t) - v - 2) { b->status = XR_PROC_OVERFLOW; goto pair_done; }
        pair = proc_alloc(b, (k + v + 2) * sizeof(wchar_t)); if (!pair) goto pair_done;
        proc_text_copy(b,pair,key,k*sizeof(wchar_t)); pair[k] = L'=';
        proc_text_copy(b,pair+k+1,value,(v+1)*sizeof(wchar_t));
        size_t slot = count;
        for (size_t j = 0; j < count; ++j) {
            size_t n;
            if (!proc_wlength(b, entries[j], &n)) break;
            if (n > k && entries[j][k] == L'=') {
                int order = CompareStringOrdinal(entries[j], (int)k, key, (int)k, TRUE);
                if (!order) { b->status = proc_text_last_error(); break; }
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
        if (n + 1 > (SIZE_MAX - bytes) / sizeof(wchar_t)) { b->status = XR_PROC_OVERFLOW; goto done; }
        bytes += (n + 1) * sizeof(wchar_t);
        for (size_t j = i; j > 0; --j) {
            size_t previous;
            if (!proc_wlength(b, entries[j - 1], &previous)) goto done;
            if (n >= INT_MAX || previous >= INT_MAX) { b->status = XR_PROC_BUDGET; goto done; }
            int order = CompareStringOrdinal(entries[j - 1], (int)previous, entries[j], (int)n, TRUE);
            if (!order) { b->status = proc_text_last_error(); goto done; }
            if (order != CSTR_GREATER_THAN) break;

            wchar_t *swap = entries[j]; entries[j] = entries[j - 1]; entries[j - 1] = swap;
        }
    }
    block = proc_alloc(b, bytes); if (!block) goto done;
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t n; if (!proc_wlength(b, entries[i], &n)) goto done;
        proc_text_copy(b,block+at,entries[i],(n+1)*sizeof(wchar_t)); at += n + 1;
    }

    block[at] = 0; block[at + 1] = 0;
 done:
    for (size_t i = 0; i < count; ++i) proc_free(b, entries[i]);
    proc_free(b, entries);
    if (inherited&&!b->inherited) FreeEnvironmentStringsW(inherited);
    if (b->status != XR_PROC_OK) { proc_free(b, block); block = NULL; }
    return block;
}

/* Debug callbacks and their borrowed handles are confined to the spawning
 * thread. Cleanup uses the same event dispatcher without invoking callbacks
 * or acquiring further resource permission. */
