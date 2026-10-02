/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_namespace_windows.inc.c - Directory R oplocks with owned completion
 */
static bool namespace_windows_error(XrXirNamespace *owner, DWORD error) {
    XrXirNamespaceStatus status = XR_XIR_NAMESPACE_IO;
    switch (error) {
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: status = XR_XIR_NAMESPACE_OUT_OF_MEMORY; break;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: status = XR_XIR_NAMESPACE_UNRESOLVED; break;
    case ERROR_INVALID_FUNCTION: case ERROR_NOT_SUPPORTED: case ERROR_OPLOCK_NOT_GRANTED:
    case ERROR_CANNOT_GRANT_REQUESTED_OPLOCK: status = XR_XIR_NAMESPACE_UNSUPPORTED; break;
    default: break;
    }
    return namespace_fail(owner, status, error);
}
static bool namespace_broken_directory(XrXirNamespace *owner, const NamespaceDirectory *node,
    uint32_t index, XrXirNamespaceFailureKind kind) {
    bool first = owner->diagnostic.status == XR_XIR_NAMESPACE_OK;
    namespace_fail(owner, XR_XIR_NAMESPACE_BROKEN, 0);
    if (first) {
        owner->diagnostic.kind = kind;
        owner->diagnostic.directory_index = index;
        owner->diagnostic.volume = node->facts.volume;
        memcpy(owner->diagnostic.file_id, node->facts.file_id, sizeof(node->facts.file_id));
    }
    return false;
}
static bool namespace_observe(XrXirNamespace *owner, NamespaceDirectory *node,
    uint32_t index, DWORD wait_ms, bool cleanup) {
    if (!node->issued || node->completed) return true;
    if (cleanup && wait_ms) {
        DWORD waited = WaitForSingleObject(node->overlapped.hEvent, wait_ms);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT)
            return namespace_windows_error(owner, GetLastError());
    }
    if (!cleanup && !namespace_work(owner, 1)) return false;
    DWORD bytes = 0;
    BOOL complete = GetOverlappedResult(node->handle, &node->overlapped, &bytes, FALSE);
    DWORD error = complete ? ERROR_SUCCESS : GetLastError();
    if (!complete && error == ERROR_IO_INCOMPLETE) return true;
    if (!cleanup && !namespace_work(owner, sizeof(node->overlapped.Internal))) return false;
    /* An API error alone says nothing about the lifetime of kernel storage. */
    if (complete || (error != ERROR_IO_INCOMPLETE && HasOverlappedIoCompleted(&node->overlapped)))
        node->completed = true;
    if (complete) return cleanup || namespace_broken_directory(owner, node, index, XR_XIR_NAMESPACE_OPLOCK_COMPLETED);
    if (cleanup && node->completed && error == ERROR_OPERATION_ABORTED) return true;
    return namespace_windows_error(owner, error);
}
static bool namespace_start(XrXirNamespace *owner, NamespaceDirectory *node) {
    const XtcXirDirectoryFacts *facts = xtc_xir_file_lease_directory_facts(node->lease);
    if (!namespace_work(owner, 1)) return false;
    node->handle = CreateFileW(facts->native_path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, NULL);
    if (node->handle == INVALID_HANDLE_VALUE) return namespace_windows_error(owner, GetLastError());
    FILE_ID_INFO identity;
    if (!namespace_work(owner, 1 + sizeof(identity))) return false;
    if (!GetFileInformationByHandleEx(node->handle, FileIdInfo, &identity, sizeof(identity)))
        return namespace_windows_error(owner, GetLastError());
    if (!namespace_work(owner, 2 * (sizeof(identity.VolumeSerialNumber) + sizeof(identity.FileId.Identifier)))) return false;
    if (identity.VolumeSerialNumber != facts->volume || memcmp(identity.FileId.Identifier, facts->file_id, 16))
        return namespace_broken_directory(owner, node, owner->facts.directory_count - 1, XR_XIR_NAMESPACE_DIRECTORY_IDENTITY);
    wchar_t filesystem[16];
    if (!namespace_work(owner, 1 + sizeof(filesystem))) return false;
    if (!GetVolumeInformationByHandleW(node->handle, NULL, 0, NULL, NULL, NULL, filesystem, 16))
        return namespace_windows_error(owner, GetLastError());
    if (!namespace_work(owner, 10 * sizeof(wchar_t))) return false;
    if (memcmp(filesystem, L"NTFS", 5 * sizeof(wchar_t))) return namespace_fail(owner, XR_XIR_NAMESPACE_UNSUPPORTED, 0);
    if (!namespace_work(owner, 1)) return false;
    node->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!node->overlapped.hEvent) return namespace_windows_error(owner, GetLastError());
    if (!namespace_work(owner, sizeof(node->input))) return false;
    node->input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    node->input.StructureLength = sizeof(node->input);
    node->input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ;
    node->input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
    if (!namespace_work(owner, 1 + sizeof(node->input) + sizeof(node->output))) return false;
    BOOL immediate = DeviceIoControl(node->handle, FSCTL_REQUEST_OPLOCK, &node->input, sizeof(node->input),
        &node->output, sizeof(node->output), NULL, &node->overlapped);
    DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
    if (immediate) return namespace_broken_directory(owner, node, owner->facts.directory_count - 1, XR_XIR_NAMESPACE_OPLOCK_IMMEDIATE);
    if (error != ERROR_IO_PENDING) return namespace_windows_error(owner, error);
    node->issued = true; return true;
}
static bool namespace_same_directory(XrXirNamespace *owner, const XtcXirDirectoryFacts *a,
    const XtcXirDirectoryFacts *b, bool *same) {
    size_t left = 0, right = 0;
    if (!namespace_length(owner, a->path, &left) || !namespace_length(owner, b->path, &right)) return false;
    if (!namespace_work(owner, left + right)) return false;
    *same = left == right && !memcmp(a->path, b->path, left);
    if (!*same) return true;
    if (!namespace_work(owner, 2 * (sizeof(a->volume) + sizeof(a->file_id)))) return false;
    return (a->volume == b->volume && !memcmp(a->file_id, b->file_id, 16)) ||
        namespace_fail(owner, XR_XIR_NAMESPACE_BROKEN, 0);
}
static bool namespace_add(XrXirNamespace *owner, XtcXirFileLease **lease, NamespaceDirectory **output) {
    const XtcXirDirectoryFacts *facts = xtc_xir_file_lease_directory_facts(*lease);
    size_t length = 0;
    if (!namespace_length(owner, facts->path, &length)) return false;
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i) {
        bool same = false;
        if (!namespace_same_directory(owner, facts, xtc_xir_file_lease_directory_facts(owner->directories[i]->lease), &same)) return false;
        if (same) { *output = owner->directories[i]; return true; }
    }
    if (owner->facts.directory_count == owner->limits.directories)
        return namespace_fail(owner, XR_XIR_NAMESPACE_BUDGET, 0);
    if (owner->facts.directory_count == owner->capacity) {
        uint32_t capacity = owner->capacity > UINT32_MAX / 2 ? owner->limits.directories : owner->capacity ? owner->capacity * 2 : 8;
        if (capacity > owner->limits.directories) capacity = owner->limits.directories;
        if ((uint64_t)capacity * sizeof(*owner->directories) > SIZE_MAX)
            return namespace_fail(owner, XR_XIR_NAMESPACE_BUDGET, 0);
        if (!namespace_resource(owner, xr_compile_resources_resize(owner->resources,
            (void **)&owner->directories, (size_t)capacity * sizeof(*owner->directories)))) return false;
        owner->capacity = capacity;
    }
    NamespaceDirectory *node = NULL;
    if (!namespace_resource(owner, xr_compile_resources_calloc(owner->resources, 1, sizeof(*node), (void **)&node))) return false;
    node->handle = INVALID_HANDLE_VALUE; node->lease = *lease; *lease = NULL;
    owner->directories[owner->facts.directory_count++] = node;
    if (!namespace_work(owner, sizeof(node->facts))) return false;
    node->facts = (XrXirNamespaceDirectoryFacts){facts->path, facts->volume, {0}, false};
    memcpy(node->facts.file_id, facts->file_id, 16);
    if (!namespace_start(owner, node)) return false;
    *output = node; return true;
}
static bool namespace_root_arm(XrXirNamespace *owner, XrXirNamespaceRootFacts *root) {
    char *candidate = NULL, *first_missing = NULL;
    XtcXirFileLease *lease = NULL;
    NamespaceDirectory *node = NULL;
    bool okay = namespace_copy_text(owner, root->requested_path, &candidate);
    while (okay) {
        XrXirTargetStatus status = xtc_xir_file_lease_directory_open(owner->resources, candidate, &lease);
        if (status == XR_XIR_TARGET_OK) break;
        if (status != XR_XIR_TARGET_UNRESOLVED) { okay = namespace_target(owner, status); break; }
        char *parent = NULL;
        okay = namespace_io(owner, xr_path_dirname_owned(&owner->io, candidate, &parent));
        if (!okay) break;
        size_t length = 0, parent_length = 0;
        okay = namespace_length(owner, candidate, &length) && namespace_length(owner, parent, &parent_length);
        if (okay && parent_length >= length) okay = namespace_fail(owner, XR_XIR_NAMESPACE_UNRESOLVED, 0);
        if (!okay) { xr_compile_resources_free(parent); break; }
        xr_compile_resources_free(first_missing); first_missing = candidate; candidate = parent;
    }
    if (okay) okay = namespace_add(owner, &lease, &node);
    if (okay && first_missing) {
        XtcXirFileLease *appeared = NULL;
        XrXirTargetStatus status = xtc_xir_file_lease_directory_open(owner->resources, first_missing, &appeared);
        xtc_xir_file_lease_free(appeared);
        if (status == XR_XIR_TARGET_OK || status == XR_XIR_TARGET_INVALID)
            okay = namespace_fail(owner, XR_XIR_NAMESPACE_BROKEN, 0);
        else if (status != XR_XIR_TARGET_UNRESOLVED) okay = namespace_target(owner, status);
    }
    if (okay && namespace_work(owner, sizeof(*root) + sizeof(node->facts.recursive) + sizeof(node->depth))) {
        root->initially_missing = first_missing != NULL; root->watched_path = node->facts.path;
        if (!first_missing && root->scope == XR_XIR_NAMESPACE_TREE) { node->facts.recursive = true; node->depth = 0; }
    }
    xr_compile_resources_free(candidate); xr_compile_resources_free(first_missing); xtc_xir_file_lease_free(lease);
    return okay && owner->diagnostic.status == XR_XIR_NAMESPACE_OK;
}
static bool namespace_expand(XrXirNamespace *owner, NamespaceDirectory *node) {
    XrDirIter *iterator = NULL;
    if (!namespace_io(owner, xr_os_io_dir_open(&owner->io, node->facts.path, &iterator))) return false;
    bool okay = true;
    while (okay) {
        XrDirEntry entry;
        XrOsIoStatus status = xr_os_io_dir_next(iterator, &entry);
        if (status == XR_OS_IO_END) break;
        if (!namespace_io(owner, status) || !namespace_work(owner, sizeof(entry.is_dir))) { okay = false; break; }
        if (!entry.is_dir) continue;
        if (node->depth == owner->limits.depth) { okay = namespace_fail(owner, XR_XIR_NAMESPACE_BUDGET, 0); break; }
        char *path = NULL;
        XtcXirFileLease *lease = NULL;
        NamespaceDirectory *child = NULL;
        okay = namespace_io(owner, xr_path_join_owned(&owner->io, node->facts.path, entry.name, &path));
        size_t length = 0;
        if (okay) okay = namespace_length(owner, path, &length);
        if (okay) okay = namespace_target(owner, xtc_xir_file_lease_directory_open(owner->resources, path, &lease));
        if (okay) okay = namespace_add(owner, &lease, &child);
        if (okay && namespace_work(owner, sizeof(child->facts.recursive) + sizeof(child->depth))) {
            if (!child->facts.recursive || child->depth > node->depth + 1) child->depth = node->depth + 1;
            child->facts.recursive = true;
        }
        xr_compile_resources_free(path); xtc_xir_file_lease_free(lease);
        if (owner->diagnostic.status != XR_XIR_NAMESPACE_OK) okay = false;
    }
    xr_os_io_dir_close(iterator);
    if (okay && namespace_work(owner, 1)) node->expanded = true;
    return okay && owner->diagnostic.status == XR_XIR_NAMESPACE_OK;
}
static bool namespace_drain(XrXirNamespace *owner, uint32_t wait_ms) {
    ULONGLONG start = GetTickCount64();
    bool okay = true;
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i) {
        NamespaceDirectory *node = owner->directories[i];
        if (!node->issued || node->completed || node->cancellation_requested) continue;
        if (CancelIoEx(node->handle, &node->overlapped)) node->cancellation_requested = true;
        else {
            DWORD error = GetLastError();
            if (error != ERROR_NOT_FOUND) { namespace_windows_error(owner, error); okay = false; }
        }
    }
    bool complete = true;
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i) {
        NamespaceDirectory *node = owner->directories[i];
        ULONGLONG elapsed = GetTickCount64() - start;
        DWORD remaining = elapsed >= wait_ms ? 0 : wait_ms - (DWORD)elapsed;
        if (!namespace_observe(owner, node, i, remaining, true)) okay = false;
        if (node->issued && !node->completed) complete = false;
    }
    if (!complete) return false;
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i) {
        NamespaceDirectory *node = owner->directories[i];
        if (node->handle != INVALID_HANDLE_VALUE) {
            if (CloseHandle(node->handle)) node->handle = INVALID_HANDLE_VALUE;
            else { namespace_windows_error(owner, GetLastError()); complete = false; }
        }
        if (node->overlapped.hEvent) {
            if (CloseHandle(node->overlapped.hEvent)) node->overlapped.hEvent = NULL;
            else { namespace_windows_error(owner, GetLastError()); complete = false; }
        }
    }
    /* Completion permits releasing kernel storage, but this call's errors must
     * remain observable on the owner before a later successful close frees it. */
    return complete && okay;
}
