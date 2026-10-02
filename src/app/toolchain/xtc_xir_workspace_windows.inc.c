/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_workspace_windows.inc.c - Identity-bound private subtree cleanup
 */
static XtcXirWorkspaceStatus workspace_error(XtcXirWorkspace *owner, DWORD error) {
    XtcXirWorkspaceStatus status = XTC_XIR_WORKSPACE_IO;
    if (error == ERROR_OUTOFMEMORY || error == ERROR_NOT_ENOUGH_MEMORY) status = XTC_XIR_WORKSPACE_OUT_OF_MEMORY;
    else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) status = XTC_XIR_WORKSPACE_UNRESOLVED;
    else if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) status = XTC_XIR_WORKSPACE_UNSUPPORTED;
    return workspace_fail(owner, status, error);
}
static bool workspace_target(XtcXirWorkspace *owner, XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return true;
    XtcXirWorkspaceStatus result = XTC_XIR_WORKSPACE_INVALID;
    switch (status) {
    case XR_XIR_TARGET_UNRESOLVED: result = XTC_XIR_WORKSPACE_UNRESOLVED; break;
    case XR_XIR_TARGET_UNSUPPORTED: result = XTC_XIR_WORKSPACE_UNSUPPORTED; break;
    case XR_XIR_TARGET_BUDGET: result = XTC_XIR_WORKSPACE_BUDGET; break;
    case XR_XIR_TARGET_OUT_OF_MEMORY: result = XTC_XIR_WORKSPACE_OUT_OF_MEMORY; break;
    case XR_XIR_TARGET_IO: result = XTC_XIR_WORKSPACE_IO; break;
    default: break;
    }
    workspace_fail(owner, result, 0); return false;
}
static NTSTATUS workspace_named_open(HANDLE parent, const wchar_t *name, USHORT bytes,
    ACCESS_MASK access, ULONG disposition, HANDLE *output) {
    UNICODE_STRING text = {bytes, bytes, (PWSTR)name};
    OBJECT_ATTRIBUTES attributes = {0};
    attributes.Length = sizeof(attributes); attributes.RootDirectory = parent;
    attributes.ObjectName = &text; attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK result = {0}; HANDLE handle = NULL;
    ULONG share = disposition == FILE_CREATE ? FILE_SHARE_READ | FILE_SHARE_WRITE :
        access & DELETE ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    ULONG options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT;
    if (disposition == FILE_CREATE) options |= FILE_DIRECTORY_FILE;
    NTSTATUS status = NtCreateFile(&handle, access | SYNCHRONIZE, &attributes, &result, NULL,
        FILE_ATTRIBUTE_NORMAL, share, disposition, options, NULL, 0);
    if (status >= 0) *output = handle;
    return status;
}
static bool workspace_identity_equal(const FILE_ID_INFO *a, const FILE_ID_INFO *b) {
    return a->VolumeSerialNumber == b->VolumeSerialNumber && !memcmp(a->FileId.Identifier, b->FileId.Identifier, 16);
}
static bool workspace_identify(XtcXirWorkspace *owner, WorkspaceNode *node) {
    if (!workspace_work(owner, 1 + sizeof(node->identity))) return false;
    if (!GetFileInformationByHandleEx(node->created, FileIdInfo, &node->identity, sizeof(node->identity))) {
        workspace_error(owner, GetLastError()); return false;
    }
    node->identified = true; return true;
}
static HANDLE workspace_parent_handle(XtcXirWorkspace *owner, uint32_t index) {
    return index ? owner->nodes[0].anchor : owner->parent_handle;
}
static bool workspace_anchor(XtcXirWorkspace *owner, uint32_t index) {
    WorkspaceNode *node = &owner->nodes[index];
    if (!workspace_work(owner, 1 + sizeof(node->anchor))) return false;
    NTSTATUS status = workspace_named_open(workspace_parent_handle(owner, index), node->name, node->name_bytes,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, &node->anchor);
    if (status < 0) { workspace_error(owner, RtlNtStatusToDosError(status)); return false; }
    FILE_ID_INFO identity = {0};
    if (!workspace_work(owner, 1 + sizeof(identity) * 3)) return false;
    if (!GetFileInformationByHandleEx(node->anchor, FileIdInfo, &identity, sizeof(identity))) {
        workspace_error(owner, GetLastError()); return false;
    }
    if (!workspace_identity_equal(&identity, &node->identity)) {
        workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0); return false;
    }
    node->anchored = true; return true;
}
static bool workspace_paths_make(XtcXirWorkspace *owner) {
    const char *parent = xtc_xir_file_lease_directory_facts(owner->parent_lease)->path;
    size_t length = 0;
    if (!workspace_length(owner, parent, &length)) return false;
    bool slash = length && (parent[length - 1] == '/' || parent[length - 1] == '\\');
    size_t root_length = length + (slash ? 0 : 1) + 37;
    if (root_length + 7 > owner->limits.path_bytes) {
        workspace_fail(owner, XTC_XIR_WORKSPACE_BUDGET, 0); return false;
    }
    if (!workspace_work(owner, 3 * (root_length + 8) + 37 * sizeof(wchar_t))) return false;
    char *root = owner->text, *input = root + owner->limits.path_bytes + 1;
    char *output = input + owner->limits.path_bytes + 1;
    memcpy(root, parent, length);
    size_t offset = length; if (!slash) root[offset++] = '/';
    for (size_t i = 0; i < 37; ++i) root[offset++] = (char)owner->nodes[0].name[i];
    root[offset] = 0;
    memcpy(input, root, offset); memcpy(input + offset, "/input", 7);
    memcpy(output, root, offset); memcpy(output + offset, "/output", 8);
    owner->paths = (XtcXirWorkspacePaths){root, input, output}; return true;
}
static XtcXirWorkspaceStatus workspace_create_windows(XtcXirWorkspace *owner) {
    if (!workspace_target(owner, xtc_xir_file_lease_directory_open(owner->resources, owner->parent, &owner->parent_lease)))
        return owner->diagnostic.status;
    const XtcXirDirectoryFacts *parent = xtc_xir_file_lease_directory_facts(owner->parent_lease);
    if (!workspace_work(owner, 1 + sizeof(owner->parent_handle))) return owner->diagnostic.status;
    owner->parent_handle = CreateFileW(parent->native_path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (owner->parent_handle == INVALID_HANDLE_VALUE) {
        owner->parent_handle = NULL; return workspace_error(owner, GetLastError());
    }
    FILE_ID_INFO identity = {0};
    if (!workspace_work(owner, 1 + sizeof(identity) * 3)) return owner->diagnostic.status;
    if (!GetFileInformationByHandleEx(owner->parent_handle, FileIdInfo, &identity, sizeof(identity)))
        return workspace_error(owner, GetLastError());
    if (identity.VolumeSerialNumber != parent->volume || memcmp(identity.FileId.Identifier, parent->file_id, 16))
        return workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
    wchar_t filesystem[16] = {0};
    if (!workspace_work(owner, 1 + sizeof(filesystem) + 10 * sizeof(wchar_t))) return owner->diagnostic.status;
    if (!GetVolumeInformationByHandleW(owner->parent_handle, NULL, 0, NULL, NULL, NULL, filesystem, 16))
        return workspace_error(owner, GetLastError());
    if (memcmp(filesystem, L"NTFS", 5 * sizeof(wchar_t))) return workspace_fail(owner, XTC_XIR_WORKSPACE_UNSUPPORTED, 0);
    for (uint32_t attempt = 0; attempt < 16; ++attempt) {
        uint8_t nonce[16]; WorkspaceNode *node = &owner->nodes[0];
        if (!workspace_work(owner, 1 + sizeof(nonce) + sizeof(node->name))) return owner->diagnostic.status;
        NTSTATUS random = BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (random < 0) return workspace_error(owner, RtlNtStatusToDosError(random));
        memcpy(node->name, L"xray-", 5 * sizeof(wchar_t));
        for (size_t i = 0; i < 16; ++i) {
            node->name[5 + i * 2] = L"0123456789abcdef"[nonce[i] >> 4];
            node->name[6 + i * 2] = L"0123456789abcdef"[nonce[i] & 15];
        }
        node->name[37] = 0; node->name_bytes = 74;
        if (!workspace_paths_make(owner) || !workspace_work(owner, 1 + sizeof(*node))) return owner->diagnostic.status;
        NTSTATUS made = workspace_named_open(owner->parent_handle, node->name, node->name_bytes,
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_CREATE, &node->created);
        if (made >= 0) { node->exists = true; break; }
        if ((ULONG)made != 0xc0000035UL) return workspace_error(owner, RtlNtStatusToDosError(made));
        if (attempt == 15) return workspace_fail(owner, XTC_XIR_WORKSPACE_IO, ERROR_ALREADY_EXISTS);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        WorkspaceNode *node = &owner->nodes[i];
        if (i) {
            if (!workspace_work(owner, sizeof(*node))) return owner->diagnostic.status;
            node->name_bytes = (USHORT)(i == 1 ? 10 : 12);
            memcpy(node->name, i == 1 ? L"input" : L"output", node->name_bytes + sizeof(wchar_t));
            if (!workspace_work(owner, 1)) return owner->diagnostic.status;
            NTSTATUS made = workspace_named_open(owner->nodes[0].anchor, node->name, node->name_bytes,
                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_CREATE, &node->created);
            if (made < 0) return workspace_error(owner, RtlNtStatusToDosError(made));
            node->exists = true;
        }
        if (!workspace_identify(owner, node) || !workspace_anchor(owner, i)) return owner->diagnostic.status;
        const char *path = i == 0 ? owner->paths.root : i == 1 ? owner->paths.input : owner->paths.output;
        if (!workspace_target(owner, xtc_xir_file_lease_directory_open(owner->resources, path, &node->lease)))
            return owner->diagnostic.status;
        const XtcXirDirectoryFacts *facts = xtc_xir_file_lease_directory_facts(node->lease);
        if (!workspace_work(owner, sizeof(identity) * 2)) return owner->diagnostic.status;
        if (facts->volume != node->identity.VolumeSerialNumber || memcmp(facts->file_id, node->identity.FileId.Identifier, 16))
            return workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
        /* Retain the root's original no-delete handle until cleanup. The
         * independent anchor permits the later identity-checked DELETE open;
         * it alone does not prevent renaming this private root. */
        if (i) {
            if (!workspace_work(owner, 1)) return owner->diagnostic.status;
            if (!CloseHandle(node->created)) return workspace_error(owner, GetLastError());
            node->created = NULL;
        }
    }
    return XTC_XIR_WORKSPACE_OK;
}
/* Each successful helper invocation below performs at most one explicit OS
 * operation. State is advanced only after that operation actually succeeds. */
static XtcXirWorkspaceStatus workspace_close_bridge(XtcXirWorkspace *owner) {
    if (owner->close_index == 3) { owner->close_index = 0; owner->close_phase = 1; return XTC_XIR_WORKSPACE_PENDING; }
    WorkspaceNode *node = &owner->nodes[owner->close_index];
    if (!node->exists) { ++owner->close_index; return XTC_XIR_WORKSPACE_PENDING; }
    if (!node->identified) {
        if (!GetFileInformationByHandleEx(node->created, FileIdInfo, &node->identity, sizeof(node->identity)))
            return workspace_error(owner, GetLastError());
        node->identified = true; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (!node->anchor) {
        NTSTATUS status = workspace_named_open(workspace_parent_handle(owner, owner->close_index), node->name, node->name_bytes,
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, &node->anchor);
        return status < 0 ? workspace_error(owner, RtlNtStatusToDosError(status)) : XTC_XIR_WORKSPACE_PENDING;
    }
    if (!node->anchored) {
        FILE_ID_INFO identity = {0};
        if (!GetFileInformationByHandleEx(node->anchor, FileIdInfo, &identity, sizeof(identity))) return workspace_error(owner, GetLastError());
        if (!workspace_identity_equal(&identity, &node->identity)) return workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
        node->anchored = true; return XTC_XIR_WORKSPACE_PENDING;
    }
    ++owner->close_index; return XTC_XIR_WORKSPACE_PENDING;
}
static XtcXirWorkspaceStatus workspace_close_leases(XtcXirWorkspace *owner) {
    if (owner->close_index == 3) { owner->close_index = 0; owner->close_phase = 2; return XTC_XIR_WORKSPACE_PENDING; }
    WorkspaceNode *node = &owner->nodes[owner->close_index];
    if (node->lease) { xtc_xir_file_lease_free(node->lease); node->lease = NULL; return XTC_XIR_WORKSPACE_PENDING; }
    if (node->created) {
        if (!CloseHandle(node->created)) return workspace_error(owner, GetLastError());
        node->created = NULL; return XTC_XIR_WORKSPACE_PENDING;
    }
    ++owner->close_index; return XTC_XIR_WORKSPACE_PENDING;
}
static XtcXirWorkspaceStatus workspace_close_fixed(XtcXirWorkspace *owner) {
    if (owner->close_index == 3) {
        if (owner->nodes[0].exists) {
            WorkspaceFrame *frame = &owner->frames[0];
            memset(frame, 0, sizeof(*frame));
            frame->handle = owner->nodes[0].deletion; owner->nodes[0].deletion = NULL;
            frame->identity = owner->nodes[0].identity; frame->directory = frame->restart = true;
            frame->fixed = 0; frame->state = 2; owner->frame_count = 1;
        }
        owner->close_phase = 3; return XTC_XIR_WORKSPACE_PENDING;
    }
    WorkspaceNode *node = &owner->nodes[owner->close_index];
    if (!node->exists) { ++owner->close_index; return XTC_XIR_WORKSPACE_PENDING; }
    if (owner->close_substage == 2) {
        if (!CloseHandle(node->deletion)) return workspace_error(owner, GetLastError());
        node->deletion = NULL; owner->close_substage = 0;
        return workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
    }
    if (!node->deletion) {
        NTSTATUS status = workspace_named_open(workspace_parent_handle(owner, owner->close_index), node->name, node->name_bytes,
            DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, &node->deletion);
        return status < 0 ? workspace_error(owner, RtlNtStatusToDosError(status)) : XTC_XIR_WORKSPACE_PENDING;
    }
    FILE_ID_INFO identity = {0};
    if (!GetFileInformationByHandleEx(node->deletion, FileIdInfo, &identity, sizeof(identity))) return workspace_error(owner, GetLastError());
    if (!workspace_identity_equal(&identity, &node->identity)) {
        owner->close_substage = 2; workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
        return XTC_XIR_WORKSPACE_PENDING;
    }
    ++owner->close_index; return XTC_XIR_WORKSPACE_PENDING;
}
static bool workspace_component(const wchar_t *name, size_t count) {
    if (!count || count > 255) return false;
    for (size_t i = 0; i < count; ++i)
        if (!name[i] || name[i] == '/' || name[i] == '\\' || name[i] == ':') return false;
    return true;
}
static XtcXirWorkspaceStatus workspace_enumerated(XtcXirWorkspace *owner, WorkspaceFrame *parent) {
    size_t offset = 0;
    for (;;) {
        size_t header = offsetof(FILE_ID_EXTD_DIR_INFO, FileName);
        if (offset > owner->limits.enum_bytes || owner->limits.enum_bytes - offset < header)
            return workspace_fail(owner, XTC_XIR_WORKSPACE_IO, ERROR_INVALID_DATA);
        FILE_ID_EXTD_DIR_INFO *entry = (void *)((uint8_t *)owner->enumeration + offset);
        if (entry->FileNameLength % 2 || entry->FileNameLength > owner->limits.enum_bytes - offset - header)
            return workspace_fail(owner, XTC_XIR_WORKSPACE_IO, ERROR_INVALID_DATA);
        size_t count = entry->FileNameLength / sizeof(wchar_t);
        bool dot = count == 1 && entry->FileName[0] == '.';
        bool dots = count == 2 && entry->FileName[0] == '.' && entry->FileName[1] == '.';
        if (!dot && !dots) {
            if (!workspace_component(entry->FileName, count)) return workspace_fail(owner, XTC_XIR_WORKSPACE_INVALID, 0);
            if (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                parent->restart = true; return workspace_fail(owner, XTC_XIR_WORKSPACE_REPARSE, 0);
            }
            if (entry->FileAttributes & FILE_ATTRIBUTE_DEVICE) return workspace_fail(owner, XTC_XIR_WORKSPACE_UNSUPPORTED, 0);
            if (owner->frame_count >= owner->limits.depth && (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                parent->restart = true; return workspace_fail(owner, XTC_XIR_WORKSPACE_BUDGET, 0);
            }
            WorkspaceFrame *child = &owner->frames[owner->frame_count]; memset(child, 0, sizeof(*child));
            child->fixed = -1; child->name_bytes = (USHORT)entry->FileNameLength;
            memcpy(child->name, entry->FileName, entry->FileNameLength); child->name[count] = 0;
            child->identity.VolumeSerialNumber = parent->identity.VolumeSerialNumber; child->identity.FileId = entry->FileId;
            child->directory = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; child->restart = true;
            if (owner->frame_count == 1) {
                for (int i = 1; i < 3; ++i) {
                    WorkspaceNode *node = &owner->nodes[i];
                    if (node->exists && workspace_identity_equal(&node->identity, &child->identity)) {
                        child->fixed = i;
                        if (node->deletion) {
                            child->handle = node->deletion; node->deletion = NULL; child->state = 1;
                        }
                    }
                }
            }
            ++owner->frame_count; return XTC_XIR_WORKSPACE_PENDING;
        }
        if (!entry->NextEntryOffset) return XTC_XIR_WORKSPACE_PENDING;
        if (entry->NextEntryOffset < header + entry->FileNameLength || entry->NextEntryOffset % 8)
            return workspace_fail(owner, XTC_XIR_WORKSPACE_IO, ERROR_INVALID_DATA);
        offset += entry->NextEntryOffset;
    }
}
static XtcXirWorkspaceStatus workspace_walk_step(XtcXirWorkspace *owner) {
    if (!owner->frame_count) { owner->close_phase = 4; return XTC_XIR_WORKSPACE_PENDING; }
    WorkspaceFrame *frame = &owner->frames[owner->frame_count - 1];
    if (frame->state == 0) {
        HANDLE parent = owner->frames[owner->frame_count - 2].handle;
        ACCESS_MASK access = DELETE | FILE_READ_ATTRIBUTES | (frame->directory ? FILE_LIST_DIRECTORY : 0);
        NTSTATUS status = workspace_named_open(parent, frame->name, frame->name_bytes, access, FILE_OPEN, &frame->handle);
        if (status < 0) return workspace_error(owner, RtlNtStatusToDosError(status));
        frame->state = 1; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (frame->state == 1) {
        FILE_ID_INFO identity = {0};
        if (!GetFileInformationByHandleEx(frame->handle, FileIdInfo, &identity, sizeof(identity))) return workspace_error(owner, GetLastError());
        if (!workspace_identity_equal(&identity, &frame->identity)) {
            frame->state = 10; frame->rejection = XTC_XIR_WORKSPACE_IDENTITY_MISMATCH;
            workspace_fail(owner, frame->rejection, 0); return XTC_XIR_WORKSPACE_PENDING;
        }
        frame->state = 2; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (frame->state == 2) {
        FILE_ATTRIBUTE_TAG_INFO attributes = {0};
        if (!GetFileInformationByHandleEx(frame->handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) return workspace_error(owner, GetLastError());
        if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            frame->state = 10; frame->rejection = XTC_XIR_WORKSPACE_REPARSE;
            workspace_fail(owner, frame->rejection, 0); return XTC_XIR_WORKSPACE_PENDING;
        }
        if (!!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != frame->directory) {
            frame->state = 10; frame->rejection = XTC_XIR_WORKSPACE_IDENTITY_MISMATCH;
            workspace_fail(owner, frame->rejection, 0); return XTC_XIR_WORKSPACE_PENDING;
        }
        frame->state = 3; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (frame->state == 3) {
        DWORD type = GetFileType(frame->handle);
        if (type != FILE_TYPE_DISK) return workspace_error(owner, type == FILE_TYPE_UNKNOWN ? GetLastError() : ERROR_NOT_SUPPORTED);
        frame->state = frame->directory ? 4 : 7; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (frame->state == 4) {
        FILE_INFO_BY_HANDLE_CLASS kind = frame->restart ? FileIdExtdDirectoryRestartInfo : FileIdExtdDirectoryInfo;
        if (!GetFileInformationByHandleEx(frame->handle, kind, owner->enumeration, owner->limits.enum_bytes)) {
            DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) return workspace_error(owner, error);
            frame->state = 7; return XTC_XIR_WORKSPACE_PENDING;
        }
        frame->restart = false; return workspace_enumerated(owner, frame);
    }
    if (frame->state == 7) {
        FILE_DISPOSITION_INFO disposition = {TRUE};
        if (!SetFileInformationByHandle(frame->handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            return workspace_error(owner, GetLastError());
        frame->state = 8; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (frame->state == 8) {
        if (frame->fixed >= 0 && owner->nodes[frame->fixed].anchor) {
            if (!CloseHandle(owner->nodes[frame->fixed].anchor)) return workspace_error(owner, GetLastError());
            owner->nodes[frame->fixed].anchor = NULL;
        }
        frame->state = 9; return XTC_XIR_WORKSPACE_PENDING;
    }
    if (!CloseHandle(frame->handle)) return workspace_error(owner, GetLastError());
    frame->handle = NULL;
    if (frame->state == 9 && frame->fixed >= 0) owner->nodes[frame->fixed].exists = false;
    XtcXirWorkspaceStatus rejection = frame->rejection;
    if (frame->state == 10 && frame->fixed == 0) {
        /* Keep the original anchor and creation ID, but release the cleanup
         * pin so external interference can be repaired before a new check. */
        owner->close_phase = 2; owner->close_index = owner->close_substage = 0;
    }
    --owner->frame_count;
    if (owner->frame_count) owner->frames[owner->frame_count - 1].restart = true;
    return rejection == XTC_XIR_WORKSPACE_OK ? XTC_XIR_WORKSPACE_PENDING : rejection;
}
static XtcXirWorkspaceStatus workspace_close_windows(XtcXirWorkspace *owner, uint32_t steps) {
    while (steps--) {
        XtcXirWorkspaceStatus status;
        if (owner->close_phase == 0) status = workspace_close_bridge(owner);
        else if (owner->close_phase == 1) status = workspace_close_leases(owner);
        else if (owner->close_phase == 2) status = workspace_close_fixed(owner);
        else if (owner->close_phase == 3) status = workspace_walk_step(owner);
        else if (owner->parent_handle) {
            if (!CloseHandle(owner->parent_handle)) return workspace_error(owner, GetLastError());
            owner->parent_handle = NULL; status = XTC_XIR_WORKSPACE_PENDING;
        } else if (owner->parent_lease) {
            xtc_xir_file_lease_free(owner->parent_lease); owner->parent_lease = NULL; status = XTC_XIR_WORKSPACE_PENDING;
        } else return XTC_XIR_WORKSPACE_OK;
        if (status != XTC_XIR_WORKSPACE_PENDING) return status;
    }
    return XTC_XIR_WORKSPACE_PENDING;
}
