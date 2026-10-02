/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_sysroot_windows.inc.c - Local Windows file input snapshots
 */
_Static_assert(XTC_XIR_TARGET_PATH_LIMIT * sizeof(wchar_t) >= 65536,
    "The input scratch buffer must cover every hash read");
static bool sysroot_length(XrXirTargetSnapshot *snapshot, const wchar_t *text, size_t *length) {
    for (size_t i = 0; i < XTC_XIR_TARGET_PATH_LIMIT; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return false;
        if (!text[i]) { *length = i; return true; }
    }
    return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET);
}
static bool sysroot_error(XrXirTargetSnapshot *snapshot, DWORD error) {
    XrXirTargetStatus status = error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ?
        XR_XIR_TARGET_OUT_OF_MEMORY : error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
        XR_XIR_TARGET_UNRESOLVED : error == ERROR_NO_UNICODE_TRANSLATION || error == ERROR_INVALID_NAME ?
        XR_XIR_TARGET_INVALID : XR_XIR_TARGET_IO;
    return xtc_xir_target_fail(snapshot, status);
}
static wchar_t *sysroot_path(XrXirTargetSnapshot *snapshot, const char *input) {
    char *text = xtc_xir_target_text(snapshot, input);
    if (!text) return NULL;
    size_t length;
    if (!xtc_xir_target_length(snapshot, text, &length) || !xtc_xir_target_work(snapshot, length < 3 ? length : 3)) return NULL;
    if (length < 3 || text[1] != ':' || (text[2] != '/' && text[2] != '\\') ||
        !((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z'))) {
        xtc_xir_target_fail(snapshot, XR_XIR_TARGET_UNSUPPORTED); return NULL;
    }
    if (length > XTC_XIR_TARGET_PATH_LIMIT * 4) { xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET); return NULL; }
    if (!xtc_xir_target_work(snapshot, length)) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, NULL, 0);
    if (count <= 0) { sysroot_error(snapshot, GetLastError()); return NULL; }
    if ((uint32_t)count + 5 >= XTC_XIR_TARGET_PATH_LIMIT) {
        xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET); return NULL;
    }
    wchar_t *path = xtc_xir_target_allocate(snapshot, ((size_t)count + 5) * sizeof(*path));
    if (!path) return NULL;
    if (!xtc_xir_target_work(snapshot, 4 * sizeof(*path) + length)) return NULL;
    memcpy(path, L"\\\\?\\", 4 * sizeof(*path));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, path + 4, count) != count) {
        sysroot_error(snapshot, GetLastError()); return NULL;
    }
    for (int i = 4; i < count + 4; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return NULL;
        if (path[i] == '/') path[i] = '\\';
    }
    size_t start = 7;
    for (size_t i = 7; i <= (size_t)count + 4; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return NULL;
        wchar_t c = path[i];
        if (!c || c == '\\') {
            size_t part = i - start;
            if (!part || path[i - 1] == '.' || path[i - 1] == ' ') {
                xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID); return NULL;
            }
            start = i + 1;
        } else if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID); return NULL;
        }
    }
    return path;
}
static bool sysroot_same_path(XrXirTargetSnapshot *snapshot, const wchar_t *left,
    const wchar_t *right, bool *same) {
    int comparison=CompareStringOrdinal(left,-1,right,-1,TRUE);
    if (!comparison) {
        DWORD error=GetLastError();
        return xtc_xir_target_fail(snapshot,error==ERROR_NOT_ENOUGH_MEMORY||error==ERROR_OUTOFMEMORY?
            XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_IO);
    }
    *same=comparison==CSTR_EQUAL;return true;
}
static bool sysroot_check_handle(XrXirTargetSnapshot *snapshot, HANDLE handle,
    const wchar_t *expected, size_t length, bool directory) {
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    BY_HANDLE_FILE_INFORMATION info = {0};
    if (!GetFileInformationByHandle(handle, &info)) return sysroot_error(snapshot, GetLastError());
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    SetLastError(NO_ERROR); DWORD kind = GetFileType(handle);
    if (kind == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) return sysroot_error(snapshot, GetLastError());
    if (kind != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    DWORD capacity = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!capacity) return sysroot_error(snapshot, GetLastError());
    if (capacity > XTC_XIR_TARGET_PATH_LIMIT) return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET);
    if (!xtc_xir_target_work(snapshot, (uint64_t)capacity * sizeof(wchar_t))) return false;
    DWORD count = GetFinalPathNameByHandleW(handle, snapshot->scratch, capacity,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count) return sysroot_error(snapshot, GetLastError());
    if (count >= capacity) return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_BUDGET);
    snapshot->scratch_length = count;
    if (!xtc_xir_target_work(snapshot, length + count + 2)) return false;
    bool same=false;
    if (!sysroot_same_path(snapshot,expected,snapshot->scratch,&same))return false;
    if (!same)
        return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
    return true;
}
static XtcXirLock *sysroot_lock(XrXirTargetSnapshot *snapshot, const wchar_t *path, size_t length, bool directory) {
    for (XtcXirLock *p = snapshot->locks; p; p = p->next) {
        if (!xtc_xir_target_work(snapshot, length + p->length + 2)) return NULL;
        bool same=false;
        if (!sysroot_same_path(snapshot,path,p->path,&same))return NULL;
        if (!same)continue;
        if (!directory) xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
        return directory ? p : NULL;
    }
    XtcXirLock *lock = xtc_xir_target_allocate(snapshot, sizeof(*lock) + (length + 1) * sizeof(*path));
    if (!lock) return NULL;
    lock->path = (wchar_t *)(lock + 1); lock->length = length;
    if (!xtc_xir_target_work(snapshot, (length + 1) * sizeof(*path))) return NULL;
    memcpy(lock->path, path, (length + 1) * sizeof(*path));
    if (!xtc_xir_target_work(snapshot, length + 1)) return NULL;
    HANDLE handle = CreateFileW(path, directory ? FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY : GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS |
        (directory ? 0 : FILE_FLAG_SEQUENTIAL_SCAN), NULL);
    if (handle == INVALID_HANDLE_VALUE) { sysroot_error(snapshot, GetLastError()); return NULL; }
    lock->handle = handle; lock->next = snapshot->locks; snapshot->locks = lock;
    if (!sysroot_check_handle(snapshot, handle, path, length, directory)) return NULL;
    return lock;
}
static const char *sysroot_canonical_text(XrXirTargetSnapshot *snapshot) {
    const wchar_t *path = snapshot->scratch + 4;
    size_t length = snapshot->scratch_length - 4;
    if (!xtc_xir_target_work(snapshot, length * sizeof(*path))) return NULL;
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, (int)length, NULL, 0, NULL, NULL);
    if (bytes <= 0) { sysroot_error(snapshot, GetLastError()); return NULL; }
    char *output = xtc_xir_target_allocate(snapshot, (size_t)bytes + 1);
    if (!output) return NULL;
    if (!xtc_xir_target_work(snapshot, length * sizeof(*path))) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, (int)length, output, bytes, NULL, NULL) != bytes) {
        sysroot_error(snapshot, GetLastError()); return NULL;
    }
    for (int i = 0; i < bytes; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return NULL;
        if (output[i] == '\\') output[i] = '/';
    }
    return output;
}
static bool sysroot_hash(XrXirTargetSnapshot *snapshot, XtcXirLock *lock, XrXirTargetFile *file) {
    LARGE_INTEGER size = {0};
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    if (!GetFileSizeEx(lock->handle, &size)) return sysroot_error(snapshot, GetLastError());
    if (size.QuadPart < 0) return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
    file->length = (uint64_t)size.QuadPart;
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    XrSHA256Context hash; xr_sha256_init(&hash);
    uint64_t left = file->length;
    while (left) {
        DWORD amount = (DWORD)(left > 65536 ? 65536 : left), actual = 0;
        if (!xtc_xir_target_work(snapshot, amount)) return false;
        if (!ReadFile(lock->handle, snapshot->scratch, amount, &actual, NULL)) return sysroot_error(snapshot, GetLastError());
        if (actual != amount) return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_IO);
        if (!xtc_xir_target_work(snapshot, actual)) return false;
        xr_sha256_update(&hash, (const uint8_t *)snapshot->scratch, actual); left -= actual;
    }
    if (!xtc_xir_target_work(snapshot, 1)) return false;
    xr_sha256_final(&hash, file->digest);
    return true;
}
static bool sysroot_file(XrXirTargetSnapshot *snapshot, const XrXirTargetDependency *input, XrXirTargetFile *file) {
    if (input->kind < XR_XIR_TARGET_COMPILER || input->kind > XR_XIR_TARGET_SOURCE)
        return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_INVALID);
    wchar_t *path = sysroot_path(snapshot, input->path);
    if (!path) return false;
    size_t length;
    if (!sysroot_length(snapshot, path, &length)) return false;
    wchar_t first = path[7]; path[7] = 0;
    XtcXirLock *root = sysroot_lock(snapshot, path, 7, true); path[7] = first;
    if (!root) return false;
    for (size_t i = 7; i < length; ++i) {
        if (!xtc_xir_target_work(snapshot, 1)) return false;
        if (path[i] != '\\') continue;
        path[i] = 0; XtcXirLock *ancestor = sysroot_lock(snapshot, path, i, true); path[i] = '\\';
        if (!ancestor) return false;
    }
    XtcXirLock *lock = sysroot_lock(snapshot, path, length, false);
    if (!lock) return false;
    file->path = sysroot_canonical_text(snapshot); file->kind = input->kind;
    if (!file->path || !sysroot_hash(snapshot, lock, file)) return false;
    return true;
}
static bool sysroot_sort_last(XrXirTargetSnapshot *snapshot, uint32_t index) {
    for (uint32_t at = index; at; --at) {
        XrXirTargetFile *a = &snapshot->files[at - 1], *b = &snapshot->files[at];
        int order;
        if (!xtc_xir_target_compare(snapshot, a->path, b->path, &order)) return false;
        if (order <= 0) break;
        if (!xtc_xir_target_work(snapshot, 3 * sizeof(*a))) return false;
        XrXirTargetFile temporary = *a; *a = *b; *b = temporary;
    }
    return true;
}
XR_FUNC bool xtc_xir_sysroot_capture(XrXirTargetSnapshot *snapshot, const XrXirTargetRequest *request) {
    snapshot->scratch = xtc_xir_target_allocate(snapshot, XTC_XIR_TARGET_PATH_LIMIT * sizeof(wchar_t));
    snapshot->files = xtc_xir_target_allocate(snapshot, request->file_count * sizeof(*snapshot->files));
    if (!snapshot->scratch || !snapshot->files) return false;
    for (uint32_t i = 0; i < request->file_count; ++i) {
        if (!sysroot_file(snapshot, &request->files[i], &snapshot->files[i])) return false;
        if (!sysroot_sort_last(snapshot, i)) return false;
    }
    return true;
}
XR_FUNC void xtc_xir_sysroot_close(XrXirTargetSnapshot *snapshot) {
    for (XtcXirLock *lock = snapshot->locks; lock; lock = lock->next)
        XR_CHECK(CloseHandle(lock->handle), "Owned target input handle could not be closed");
}
