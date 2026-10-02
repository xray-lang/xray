/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_publication.c - Same-handle output staging and named replacement
 */
#include "xtc_xir_publication.h"
#include "xtc_xir_file_lease.h"
#include "../../base/xio_policy.inc.h"
#include "../../shared/xr_path_limit.h"
#if XR_OS_WINDOWS
#include "../../base/xwindows_utf8.h"
#include <bcrypt.h>
#include <stddef.h>
#endif

struct XtcXirPublication {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    XtcXirPublicationLimits limits;
    XtcXirPublicationDiagnostic diagnostic;
    XtcXirPublicationPhase phase;
    XtcXirFileLease *parent_lease;
    char *parent;
#if XR_OS_WINDOWS
    HANDLE file, parent_handle;
    wchar_t *absolute, *temporary, *leaf;
    FILE_RENAME_INFO *rename;
    size_t capacity, leaf_units, leaf_bytes;
    wchar_t drive[4];
    DWORD rename_size;
    bool delete_pending;
#endif
};
static XtcXirPublicationStatus publication_fail(XtcXirPublication *owner,
    XtcXirPublicationStatus status, uint32_t error) {
    if (owner->diagnostic.status == XTC_XIR_PUBLICATION_OK) {
        owner->diagnostic.status = status; owner->diagnostic.os_error = error;
    }
    if (!owner->diagnostic.published) owner->phase = XTC_XIR_PUBLICATION_FAILED;
    return owner->diagnostic.status;
}
static XtcXirPublicationStatus publication_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XTC_XIR_PUBLICATION_OK;
    case XR_OS_IO_NOT_FOUND: return XTC_XIR_PUBLICATION_UNRESOLVED;
    case XR_OS_IO_UNSUPPORTED: return XTC_XIR_PUBLICATION_UNSUPPORTED;
    case XR_OS_IO_BAD_ARGUMENT: return XTC_XIR_PUBLICATION_INVALID;
    case XR_OS_IO_BUDGET: return XTC_XIR_PUBLICATION_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XTC_XIR_PUBLICATION_OUT_OF_MEMORY;
    default: return XTC_XIR_PUBLICATION_IO;
    }
}
static bool publication_io(XtcXirPublication *owner, XrOsIoStatus status) {
    if (status != XR_OS_IO_OK) publication_fail(owner, publication_io_status(status), 0);
    return owner->diagnostic.status == XTC_XIR_PUBLICATION_OK;
}
static bool publication_work(XtcXirPublication *owner, uint64_t units) {
    return owner->diagnostic.status == XTC_XIR_PUBLICATION_OK &&
        publication_io(owner, owner->policy.work(owner->policy.context, units));
}
static void *publication_alloc(XtcXirPublication *owner, size_t size) {
    void *memory = NULL;
    if (owner->diagnostic.status == XTC_XIR_PUBLICATION_OK)
        publication_io(owner, owner->policy.alloc(owner->policy.context, size, &memory));
    return memory;
}
static void publication_free(XtcXirPublication *owner) {
    xtc_xir_file_lease_free(owner->parent_lease);
    xr_compile_resources_free(owner->parent);
#if XR_OS_WINDOWS
    xr_compile_resources_free(owner->absolute); xr_compile_resources_free(owner->temporary);
    xr_compile_resources_free(owner->leaf); xr_compile_resources_free(owner->rename);
#endif
    xr_compile_resources_free(owner);
}
#if XR_OS_WINDOWS
static bool publication_windows_error(XtcXirPublication *owner, DWORD error) {
    publication_fail(owner, publication_io_status(io_windows_status(error)), error); return false;
}
static bool publication_copy(XtcXirPublication *owner, void *out, const void *in, size_t bytes) {
    if (!publication_work(owner, bytes)) return false;
    if (bytes) memcpy(out, in, bytes);
    return true;
}
static bool publication_text_length(XtcXirPublication *owner, const char *text, size_t *length) {
    for (size_t i = 0; i <= owner->limits.path_bytes; ++i) {
        if (!publication_work(owner, 1)) return false;
        if (!text[i]) { *length = i; return true; }
    }
    publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0); return false;
}
static bool publication_prepare(XtcXirPublication *owner, const char *destination) {
    size_t text_length = 0;
    if (!publication_text_length(owner, destination, &text_length)) return false;
    if (!text_length) { publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0); return false; }
    wchar_t *spelling = NULL, *full = NULL; char *normalized = NULL;
    size_t spelling_units = 0;
    XrIoContext io = {&owner->policy, XR_OS_IO_OK};
    if (!publication_io(owner, xr_win_utf8_owned(&owner->policy, destination, &spelling, &spelling_units))) goto done;
    if (!xr_win_path_components(&io, spelling, spelling_units)) {
        publication_io(owner, io.status); goto done;
    }
    DWORD needed = 0;
    if (publication_work(owner, 1 + (spelling_units + 1) * sizeof(wchar_t))) {
        needed = GetFullPathNameW(spelling, 0, NULL, NULL);
        if (!needed) publication_windows_error(owner, GetLastError());
        else if (needed > 32768) publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0);
    }
    full = publication_alloc(owner, (size_t)needed * sizeof(wchar_t));
    DWORD count = 0;
    if (full && publication_work(owner, 1 + (spelling_units + 1 + (size_t)needed) * sizeof(wchar_t))) {
        count = GetFullPathNameW(spelling, needed, full, NULL);
        if (!count) publication_windows_error(owner, GetLastError());
        else if (count >= needed) publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0);
    }
    if (!publication_work(owner, 3 * sizeof(wchar_t))) goto done;
    if (count < 3 || !((full[0] >= L'A' && full[0] <= L'Z') || (full[0] >= L'a' && full[0] <= L'z')) ||
        full[1] != L':' || full[2] != L'\\') {
        publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0); goto done;
    }
    size_t separator = 2, component = 3;
    for (size_t i = 3; i <= count; ++i) {
        if (!publication_work(owner, sizeof(wchar_t))) goto done;
        wchar_t c = full[i];
        if (c == L':' || c == L'/' || c == L'*' || c == L'?') {
            publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0); goto done;
        }
        if (!c || c == L'\\') {
            if (!xr_win_path_component(&io, full + component, i - component)) {
                publication_io(owner, io.status); goto done;
            }
            if (c) { separator = i; component = i + 1; }
        }
    }
    if (separator + 1 == count) { publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0); goto done; }
    if (!publication_io(owner, xr_win_utf16_text_owned(&owner->policy, full, &normalized)) ||
        !publication_text_length(owner, normalized, &text_length)) goto done;
    for (size_t i = text_length; i; --i) {
        if (!publication_work(owner, 1)) goto done;
        if (normalized[i - 1] == '\\') { owner->leaf_bytes = text_length - i; break; }
    }
    if (!publication_copy(owner, owner->drive, full, 3 * sizeof(wchar_t)) ||
        !publication_work(owner, sizeof(wchar_t))) goto done;
    owner->drive[3] = 0;
    owner->capacity = (size_t)owner->limits.path_bytes + 5;
    owner->absolute = publication_alloc(owner, owner->capacity * sizeof(wchar_t));
    owner->temporary = publication_alloc(owner, owner->capacity * sizeof(wchar_t));
    owner->rename = publication_alloc(owner, offsetof(FILE_RENAME_INFO, FileName) + owner->capacity * sizeof(wchar_t));
    owner->leaf_units = count - separator - 1;
    owner->leaf = publication_alloc(owner, (owner->leaf_units + 1) * sizeof(wchar_t));
    if (!publication_copy(owner, owner->leaf, full + separator + 1, (owner->leaf_units + 1) * sizeof(wchar_t))) goto done;
    if (!publication_work(owner, sizeof(wchar_t))) goto done;
    full[separator == 2 ? 3 : separator] = 0;
    publication_io(owner, xr_win_utf16_text_owned(&owner->policy, full, &owner->parent));
done:
    xr_compile_resources_free(normalized); xr_compile_resources_free(full); xr_compile_resources_free(spelling);
    return owner->diagnostic.status == XTC_XIR_PUBLICATION_OK;
}
static bool publication_parent(XtcXirPublication *owner, size_t *parent_units) {
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_PARENT;
    XrXirTargetStatus leased = xtc_xir_file_lease_directory_open(owner->resources, owner->parent, &owner->parent_lease);
    if (leased != XR_XIR_TARGET_OK) {
        XtcXirPublicationStatus status = leased == XR_XIR_TARGET_BUDGET ? XTC_XIR_PUBLICATION_BUDGET :
            leased == XR_XIR_TARGET_OUT_OF_MEMORY ? XTC_XIR_PUBLICATION_OUT_OF_MEMORY :
            leased == XR_XIR_TARGET_UNRESOLVED ? XTC_XIR_PUBLICATION_UNRESOLVED :
            leased == XR_XIR_TARGET_UNSUPPORTED ? XTC_XIR_PUBLICATION_UNSUPPORTED :
            leased == XR_XIR_TARGET_IO ? XTC_XIR_PUBLICATION_IO : XTC_XIR_PUBLICATION_INVALID;
        publication_fail(owner, status, 0); return false;
    }
    const XtcXirDirectoryFacts *facts = xtc_xir_file_lease_directory_facts(owner->parent_lease);
    size_t parent_bytes = 0;
    if (!publication_text_length(owner, facts->path, &parent_bytes) ||
        !publication_work(owner, 1)) return false;
    if (GetDriveTypeW(owner->drive) != DRIVE_FIXED) {
        publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0); return false;
    }
    if (parent_bytes + owner->leaf_bytes + 1 > owner->limits.path_bytes ||
        parent_bytes + 43 > owner->limits.path_bytes) {
        publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0); return false;
    }
    size_t units = 0;
    while (publication_work(owner, sizeof(wchar_t))) {
        if (!facts->native_path[units]) break;
        if (++units == owner->capacity) { publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0); return false; }
    }
    if (!publication_work(owner, sizeof(wchar_t))) return false;
    bool slash = units && facts->native_path[units - 1] == L'\\';
    size_t prefix = units + (slash ? 0 : 1);
    if (prefix + owner->leaf_units + 1 > owner->capacity || prefix + 43 > owner->capacity ||
        prefix + owner->leaf_units >= 32768 || prefix + 42 >= 32768) {
        publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0); return false;
    }
    if (!publication_copy(owner, owner->absolute, facts->native_path, units * sizeof(wchar_t)) ||
        !publication_work(owner, sizeof(wchar_t))) return false;
    if (!slash) owner->absolute[units] = L'\\';
    if (!publication_copy(owner, owner->absolute + prefix, owner->leaf, (owner->leaf_units + 1) * sizeof(wchar_t)) ||
        !publication_copy(owner, owner->temporary, owner->absolute, prefix * sizeof(wchar_t))) return false;
    if (!publication_work(owner, 1)) return false;
    owner->parent_handle = CreateFileW(facts->native_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (owner->parent_handle == INVALID_HANDLE_VALUE) return publication_windows_error(owner, GetLastError());
    wchar_t filesystem[16];
    if (!publication_work(owner, 1 + sizeof(filesystem))) return false;
    if (!GetVolumeInformationByHandleW(owner->parent_handle, NULL, 0, NULL, NULL, NULL, filesystem, 16))
        return publication_windows_error(owner, GetLastError());
    if (!publication_work(owner, sizeof(L"NTFS") * 2)) return false;
    if (memcmp(filesystem, L"NTFS", sizeof(L"NTFS"))) {
        publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0); return false;
    }
    size_t name_bytes = (prefix + owner->leaf_units) * sizeof(wchar_t);
    if (!publication_work(owner, offsetof(FILE_RENAME_INFO, FileName))) return false;
    memset(owner->rename, 0, offsetof(FILE_RENAME_INFO, FileName));
    owner->rename->ReplaceIfExists = TRUE; owner->rename->FileNameLength = (DWORD)name_bytes;
    if (!publication_copy(owner, owner->rename->FileName, owner->absolute, name_bytes + sizeof(wchar_t))) return false;
    owner->rename_size = (DWORD)(offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t));
    *parent_units = prefix; return true;
}
static bool publication_destination(XtcXirPublication *owner) {
    if (!publication_work(owner, 1)) return false;
    DWORD attributes = GetFileAttributesW(owner->absolute);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? true : publication_windows_error(owner, error);
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
        publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0); return false;
    }
    return true;
}
static bool publication_create(XtcXirPublication *owner, size_t prefix) {
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_CREATE;
    static const wchar_t hex[] = L"0123456789abcdef";
    for (uint32_t attempt = 0; attempt < owner->limits.create_attempts; ++attempt) {
        unsigned char nonce[16];
        if (!publication_work(owner, 1 + sizeof(nonce))) return false;
        NTSTATUS random = BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (random < 0) {
            publication_fail(owner, (uint32_t)random == UINT32_C(0xc0000017) ?
                XTC_XIR_PUBLICATION_OUT_OF_MEMORY : XTC_XIR_PUBLICATION_IO, (uint32_t)random); return false;
        }
        if (!publication_copy(owner, owner->temporary + prefix, L".xray-", 6 * sizeof(wchar_t))) return false;
        for (size_t i = 0; i < sizeof(nonce); ++i) {
            if (!publication_work(owner, 1 + 4 * sizeof(wchar_t))) return false;
            unsigned char value = nonce[i];
            owner->temporary[prefix + 6 + i * 2] = hex[value >> 4];
            owner->temporary[prefix + 7 + i * 2] = hex[value & 15];
        }
        if (!publication_copy(owner, owner->temporary + prefix + 38, L".tmp", 5 * sizeof(wchar_t)) ||
            !publication_work(owner, 1)) return false;
        owner->file = CreateFileW(owner->temporary, GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
            0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (owner->file != INVALID_HANDLE_VALUE) return true;
        DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return publication_windows_error(owner, error);
    }
    publication_fail(owner, XTC_XIR_PUBLICATION_IO, ERROR_FILE_EXISTS); return false;
}
#endif
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_new(XrCompileResources *resources,
    const XtcXirPublicationRequest *request, XtcXirPublication **output) {
    if (!resources || !request || !request->destination || !output || *output) return XTC_XIR_PUBLICATION_INVALID;
    if (!request->limits.path_bytes || request->limits.path_bytes > 32767 ||
        !request->limits.create_attempts || request->limits.create_attempts > 64 ||
        !request->limits.max_bytes || request->limits.max_bytes >= SIZE_MAX) return XTC_XIR_PUBLICATION_BUDGET;
#if !XR_OS_WINDOWS
    return XTC_XIR_PUBLICATION_UNSUPPORTED;
#else
    XtcXirPublication *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK) return allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ?
        XTC_XIR_PUBLICATION_OUT_OF_MEMORY : XTC_XIR_PUBLICATION_BUDGET;
    owner->resources = resources; owner->policy = xr_compile_io_policy(resources);
    owner->file = owner->parent_handle = INVALID_HANDLE_VALUE;
    if (!publication_work(owner, sizeof(request->limits))) {
        XtcXirPublicationStatus status = owner->diagnostic.status; publication_free(owner); return status;
    }
    owner->limits = request->limits;
    if (!publication_prepare(owner, request->destination) || !publication_work(owner, sizeof(*output))) {
        XtcXirPublicationStatus status = owner->diagnostic.status; publication_free(owner); return status;
    }
    *output = owner; return XTC_XIR_PUBLICATION_OK;
#endif
}
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_write(XtcXirPublication *owner,
    const void *bytes, size_t length) {
    if (!owner) return XTC_XIR_PUBLICATION_INVALID;
    if (owner->diagnostic.status != XTC_XIR_PUBLICATION_OK) return owner->diagnostic.status;
    if (owner->phase != XTC_XIR_PUBLICATION_NEW || (!bytes && length))
        return publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0);
    if (length > owner->limits.max_bytes) return publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0);
#if !XR_OS_WINDOWS
    return publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0);
#else
    owner->phase = XTC_XIR_PUBLICATION_WRITING;
    size_t prefix = 0;
    if (!publication_parent(owner, &prefix) || !publication_destination(owner) || !publication_create(owner, prefix))
        return owner->diagnostic.status;
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_WRITE;
    size_t offset = 0;
    while (offset < length) {
        DWORD count = 0, chunk = length - offset > MAXDWORD ? MAXDWORD : (DWORD)(length - offset);
        if (!publication_work(owner, 1 + (uint64_t)chunk)) break;
        if (!WriteFile(owner->file, (const uint8_t *)bytes + offset, chunk, &count, NULL)) {
            publication_windows_error(owner, GetLastError()); break;
        }
        if (!count || count > chunk) { publication_fail(owner, XTC_XIR_PUBLICATION_IO, 0); break; }
        offset += count;
    }
    if (owner->diagnostic.status != XTC_XIR_PUBLICATION_OK) return owner->diagnostic.status;
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_FLUSH;
    if (!publication_work(owner, 1)) return owner->diagnostic.status;
    if (!FlushFileBuffers(owner->file)) { publication_windows_error(owner, GetLastError()); return owner->diagnostic.status; }
    owner->phase = XTC_XIR_PUBLICATION_READY; return XTC_XIR_PUBLICATION_OK;
#endif
}
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_commit(XtcXirPublication *owner) {
    if (!owner) return XTC_XIR_PUBLICATION_INVALID;
    if (owner->diagnostic.status != XTC_XIR_PUBLICATION_OK) return owner->diagnostic.status;
    if (owner->phase != XTC_XIR_PUBLICATION_READY) return publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0);
#if !XR_OS_WINDOWS
    return publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0);
#else
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_RENAME;
    if (!publication_destination(owner) || !publication_work(owner, 1 + owner->rename_size)) return owner->diagnostic.status;
    if (!SetFileInformationByHandle(owner->file, FileRenameInfo, owner->rename, owner->rename_size)) {
        publication_windows_error(owner, GetLastError()); return owner->diagnostic.status;
    }
    owner->diagnostic.published = true; owner->phase = XTC_XIR_PUBLICATION_PUBLISHED;
    return XTC_XIR_PUBLICATION_OK;
#endif
}
XR_FUNC XtcXirPublicationPhase xtc_xir_publication_phase(const XtcXirPublication *owner) {
    return owner ? owner->phase : XTC_XIR_PUBLICATION_FAILED;
}
XR_FUNC const XtcXirPublicationDiagnostic *xtc_xir_publication_diagnostic(const XtcXirPublication *owner) {
    return owner ? &owner->diagnostic : NULL;
}
#if XR_OS_WINDOWS
static XtcXirPublicationStatus publication_pending(XtcXirPublication *owner, DWORD error) {
    owner->diagnostic.cleanup_pending = true; owner->diagnostic.cleanup_os_error = error;
    if (owner->diagnostic.status == XTC_XIR_PUBLICATION_OK) {
        owner->diagnostic.stage = XTC_XIR_PUBLICATION_CLEANUP;
        publication_windows_error(owner, error);
    }
    return XTC_XIR_PUBLICATION_PENDING;
}
#endif
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_close(XtcXirPublication **slot) {
    if (!slot) return XTC_XIR_PUBLICATION_INVALID;
    XtcXirPublication *owner = *slot;
    if (!owner) return XTC_XIR_PUBLICATION_OK;
#if XR_OS_WINDOWS
    if (owner->file != INVALID_HANDLE_VALUE) {
        if (!owner->diagnostic.published && !owner->delete_pending) {
            FILE_DISPOSITION_INFO disposition = {TRUE};
            if (!SetFileInformationByHandle(owner->file, FileDispositionInfo, &disposition, sizeof(disposition)))
                return publication_pending(owner, GetLastError());
            owner->delete_pending = true;
        }
        if (!CloseHandle(owner->file)) return publication_pending(owner, GetLastError());
        owner->file = INVALID_HANDLE_VALUE;
    }
    if (owner->parent_handle != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(owner->parent_handle)) return publication_pending(owner, GetLastError());
        owner->parent_handle = INVALID_HANDLE_VALUE;
    }
#endif
    publication_free(owner); *slot = NULL; return XTC_XIR_PUBLICATION_OK;
}
