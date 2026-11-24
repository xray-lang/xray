/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * workspace_disposition.inc.c - Real identity-bound image deletion failures
 */
typedef enum DispositionCase {
    DISPOSITION_RELEASE_IMAGE, DISPOSITION_KEEP_IMAGE, DISPOSITION_READONLY, DISPOSITION_NO_DELETE
} DispositionCase;

static WorkspaceFrame *disposition_frame(XtcXirWorkspace **owner) {
    for (unsigned calls = 0; calls < 256; ++calls) {
        XtcXirWorkspace *workspace = *owner;
        if (workspace->close_phase == 3 && workspace->frame_count > 1) {
            WorkspaceFrame *frame = &workspace->frames[workspace->frame_count - 1];
            if (frame->fixed < 0 && !frame->directory && frame->state == WORKSPACE_DISPOSITION &&
                !wcscmp(frame->name, L"held.exe")) return frame;
        }
        CHECK(xtc_xir_workspace_close(owner, 1) == XTC_XIR_WORKSPACE_PENDING && *owner);
    }
    CHECK(false);
    return NULL;
}
static HANDLE disposition_open(const char *path, DWORD access, const FILE_ID_INFO *expected) {
    HANDLE file = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    FILE_ID_INFO found = fixture_identity(file);
    CHECK(workspace_identity_equal(expected, &found));
    return file;
}
static XtcXirWorkspaceStatus disposition_close_step(XtcXirWorkspace **owner) {
    size_t before = io_count;
    XtcXirWorkspaceStatus status = xtc_xir_workspace_close(owner, 1);
    CHECK(io_count == before + 1);
    return status;
}
static void disposition_image_case(XtcXirWorkspace **owner, WorkspaceFrame *frame, const char *path, bool persistent) {
    HANDLE original = frame->handle;
    HANDLE file = disposition_open(path, GENERIC_READ, &frame->identity);
    HANDLE image = CreateFileMappingW(file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
    CHECK(image);
    void *view = MapViewOfFile(image, FILE_MAP_READ, 0, 0, 0);
    CHECK(view && CloseHandle(file));
    unsigned retries = persistent ? WORKSPACE_MAX_DISPOSITION_RETRIES : 1;
    for (unsigned i = 0; i < retries; ++i) {
        CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_PENDING);
        CHECK((ULONG)last_native_disposition == 0xc0000121UL && frame->state == WORKSPACE_RECHECK_DISPOSITION);
        CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_DISPOSITION_RETRY);
        CHECK(frame->handle == original && frame->state == WORKSPACE_DISPOSITION);
        CHECK(xtc_xir_workspace_status(*owner) == XTC_XIR_WORKSPACE_OK);
    }
    if (persistent) {
        CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_IO);
        CHECK((ULONG)last_native_disposition == 0xc0000121UL);
    }
    CHECK(UnmapViewOfFile(view) && CloseHandle(image));
    FILE_ID_INFO same = fixture_identity(original);
    CHECK(frame->handle == original && workspace_identity_equal(&frame->identity, &same));
    CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_PENDING && frame->state == 8);
}

static void disposition_hard_case(XtcXirWorkspace **owner, WorkspaceFrame *frame, const char *path, bool readonly) {
    HANDLE original = frame->handle;
    HANDLE inspect = disposition_open(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, &frame->identity);
    if (readonly) {
        FILE_BASIC_INFO attributes = {.FileAttributes = FILE_ATTRIBUTE_READONLY};
        CHECK(SetFileInformationByHandle(inspect, FileBasicInfo, &attributes, sizeof(attributes)));
        CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_PENDING);
        CHECK((ULONG)last_native_disposition == 0xc0000121UL);
        CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_IO && frame->handle == original);
        attributes.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        CHECK(SetFileInformationByHandle(inspect, FileBasicInfo, &attributes, sizeof(attributes)));
    } else {
        /* The real NT call gets a same-file handle without DELETE access. */
        frame->handle = inspect;
        CHECK(disposition_close_step(owner) == XTC_XIR_WORKSPACE_IO);
        CHECK((ULONG)last_native_disposition == 0xc0000022UL);
        frame->handle = original;
    }
    CHECK((*owner)->disposition_retries == 0 && CloseHandle(inspect));
}
static void disposition_case(const char *parent, const char *program, DispositionCase mode) {
    DWORD handles = handle_count();
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    uint64_t held = sdk_stats(resources).held_bytes;
    XtcXirWorkspaceRequest request = request_for(parent);
    XtcXirWorkspace *owner = NULL;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    char path[8192], root[8192];
    strcpy_s(root, sizeof(root), xtc_xir_workspace_paths(owner)->root);
    path_join(path, sizeof(path), xtc_xir_workspace_paths(owner)->output, "held.exe");
    CHECK(CopyFileA(program, path, TRUE));
    WorkspaceFrame *frame = disposition_frame(&owner);
    if (mode == DISPOSITION_RELEASE_IMAGE || mode == DISPOSITION_KEEP_IMAGE)
        disposition_image_case(&owner, frame, path, mode == DISPOSITION_KEEP_IMAGE);
    else disposition_hard_case(&owner, frame, path, mode == DISPOSITION_READONLY);
    const XtcXirWorkspaceDiagnostic *diagnostic = xtc_xir_workspace_diagnostic(owner);
    if (mode == DISPOSITION_RELEASE_IMAGE) CHECK(diagnostic->status == XTC_XIR_WORKSPACE_OK);
    else {
        CHECK(diagnostic->status == XTC_XIR_WORKSPACE_IO && diagnostic->os_error == ERROR_ACCESS_DENIED);
        CHECK(diagnostic->during_cleanup && diagnostic->cleanup_phase == 3 && diagnostic->cleanup_depth == 3);
        if (mode == DISPOSITION_READONLY) {
            CHECK(diagnostic->cleanup_step == WORKSPACE_RECHECK_DISPOSITION && !diagnostic->nt_status);
        } else {
            uint32_t expected = mode == DISPOSITION_KEEP_IMAGE ? UINT32_C(0xc0000121) : UINT32_C(0xc0000022);
            CHECK(diagnostic->cleanup_step == WORKSPACE_DISPOSITION && diagnostic->nt_status == expected);
        }
    }
    drain(&owner, 1);
    CHECK(GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES && sdk_stats(resources).held_bytes == held);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes && !owner_handle_count);
    if (mode != DISPOSITION_RELEASE_IMAGE) CHECK(handle_count() == handles);
}
static DWORD disposition_cases(const char *parent) {
    char program[8192];
    DWORD length = GetModuleFileNameA(NULL, program, sizeof(program));
    CHECK(length && length < sizeof(program));
    disposition_case(parent, program, DISPOSITION_RELEASE_IMAGE);
    /* The first image mapping may initialize persistent Windows services. */
    DWORD warmed = handle_count();
    disposition_case(parent, program, DISPOSITION_RELEASE_IMAGE);
    CHECK(handle_count() == warmed);
    disposition_case(parent, program, DISPOSITION_KEEP_IMAGE);
    disposition_case(parent, program, DISPOSITION_READONLY);
    disposition_case(parent, program, DISPOSITION_NO_DELETE);
    puts("native image deletion, persistent mapped files, readonly and NT access denial PASS");
    return warmed;
}
