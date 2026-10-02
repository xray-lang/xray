/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlockfile.c - Policy-owned lock records, serialization and artifact checksums
 */
#include "xlockfile.h"
#include "../base/xio_policy.inc.h"
#include "../base/xsha256.h"
#include "../os/os_fs.h"
#include "../os/os_proc.h"
#include <stdatomic.h>
#include <ctype.h>
#include <limits.h>

#define LOCKFILE_VERSION 1
#define INITIAL_CAPACITY 16
XR_FUNC bool xr_lockfile_uses_policy(const XrLockfile *lock, const XrOsIoPolicy *policy) {
    return lock && io_policy_valid(policy) && lock->policy.context == policy->context &&
        lock->policy.alloc == policy->alloc && lock->policy.free == policy->free && lock->policy.work == policy->work;
}
static unsigned char lock_character(XrIoContext *io, const char *text) {
    return io_work(io, 1) ? (unsigned char)*text : 0;
}
static bool equal_text(XrIoContext *io, const char *a, const char *b) {
    if (!a || !b) return a == b;
    for (;;) {
        unsigned char ac = lock_character(io, a++), bc = lock_character(io, b++);
        if (!io_work(io, 1) || ac != bc) return false;
        if (!ac) return true;
    }
}
static char *copy_text(XrIoContext *io, const char *text) {
    if (!text) return NULL;
    size_t length = 0; if (!io_length(io, text, &length)) return NULL;
    char *result = io_alloc(io, length + 1);
    if (result && !io_copy(io, result, text, length + 1)) { io_free(io, result); return NULL; }
    return result;
}
static void free_dependencies(const XrOsIoPolicy *policy, char **dependencies, int count) {
    for (int i = 0; i < count; ++i) if (dependencies[i]) policy->free(policy->context, dependencies[i]);
    if (dependencies) policy->free(policy->context, dependencies);
}
static void free_package(const XrOsIoPolicy *policy, XrLockedPackage *package) {
    if (package->name) policy->free(policy->context, package->name);
    if (package->version) policy->free(policy->context, package->version);
    if (package->resolved) policy->free(policy->context, package->resolved);
    if (package->checksum) policy->free(policy->context, package->checksum);
    free_dependencies(policy, package->dependencies, package->dep_count);
}
XR_FUNC void xr_lockfile_free_owned(XrLockfile *lock) {
    if (!lock) return;
    XrOsIoPolicy policy = lock->policy;
    for (int i = 0; i < lock->package_count; ++i) free_package(&policy, &lock->packages[i]);
    if (lock->packages) policy.free(policy.context, lock->packages);
    policy.free(policy.context, lock);
}
XR_FUNC XrOsIoStatus xr_lockfile_new_owned(const XrOsIoPolicy *policy, XrLockfile **output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; XrLockfile *lock = io_alloc(&io, sizeof(*lock));
    if (!lock) return io.status;
    if (!io_clear(&io, lock, sizeof(*lock))) { io_free(&io, lock); return io.status; }
    lock->policy = *policy; lock->version = LOCKFILE_VERSION; lock->package_capacity = INITIAL_CAPACITY;
    lock->packages = io_alloc(&io, INITIAL_CAPACITY * sizeof(*lock->packages));
    if (lock->packages) io_clear(&io, lock->packages, INITIAL_CAPACITY * sizeof(*lock->packages));
    if (io_work(&io, sizeof(*output))) *output = lock;
    else xr_lockfile_free_owned(lock);
    return io.status;
}
static int find_index(XrIoContext *io, const XrLockfile *lock, const char *name) {
    for (int i = 0; i < lock->package_count && io_work(io, 1); ++i)
        if (equal_text(io, lock->packages[i].name, name)) return i;
    return -1;
}
XR_FUNC XrOsIoStatus xr_lockfile_find_owned(const XrOsIoPolicy *policy, const XrLockfile *lock,
    const char *name, const XrLockedPackage **output) {
    if (!xr_lockfile_uses_policy(lock, policy) || !name || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int index = find_index(&io, lock, name);
    if (io.status != XR_OS_IO_OK) return io.status;
    if (index < 0) return XR_OS_IO_NOT_FOUND;
    if (io_work(&io, sizeof(*output))) *output = &lock->packages[index];
    return io.status;
}
XR_FUNC XrOsIoStatus xr_lockfile_has_owned(const XrOsIoPolicy *policy, const XrLockfile *lock,
    const char *name, bool *output) {
    if (!xr_lockfile_uses_policy(lock, policy) || !name || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; bool found = find_index(&io, lock, name) >= 0;
    if (io_work(&io, sizeof(*output))) *output = found;
    return io.status;
}
static void *grow_array(XrIoContext *io, const void *old, int count, int capacity, size_t item_size, int *next) {
    if (capacity > INT_MAX / 2) { io_status(io, XR_OS_IO_BUDGET); return NULL; }
    int wanted = capacity < 4 ? 4 : capacity * 2;
    if ((size_t)wanted > SIZE_MAX / item_size) { io_status(io, XR_OS_IO_BUDGET); return NULL; }
    void *result = io_alloc(io, (size_t)wanted * item_size);
    if (result && !io_copy(io, result, old, (size_t)count * item_size)) { io_free(io, result); return NULL; }
    *next = wanted; return result;
}
XR_FUNC XrOsIoStatus xr_lockfile_add_package_owned(XrLockfile *lock, const char *name,
    const char *version, const char *resolved, const char *checksum) {
    if (!lock || !io_policy_valid(&lock->policy) || !name) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&lock->policy, XR_OS_IO_OK}; int index = find_index(&io, lock, name);
    XrLockedPackage pending = {0};
    if (index < 0) pending.name = copy_text(&io, name);
    pending.version = copy_text(&io, version); pending.resolved = copy_text(&io, resolved); pending.checksum = copy_text(&io, checksum);
    XrLockedPackage *expanded = NULL; int capacity = lock->package_capacity;
    if (index < 0 && lock->package_count == capacity && io.status == XR_OS_IO_OK)
        expanded = grow_array(&io, lock->packages, lock->package_count, capacity, sizeof(*expanded), &capacity);
    if (io_work(&io, sizeof(pending))) {
        if (index >= 0) {
            XrLockedPackage *package = &lock->packages[index];
            io_free(&io, package->version); io_free(&io, package->resolved); io_free(&io, package->checksum);
            package->version = pending.version; package->resolved = pending.resolved; package->checksum = pending.checksum;
        } else {
            if (expanded) { io_free(&io, lock->packages); lock->packages = expanded; lock->package_capacity = capacity; expanded = NULL; }
            lock->packages[lock->package_count++] = pending;
        }
        return XR_OS_IO_OK;
    }
    io_free(&io, expanded); free_package(io.policy, &pending); return io.status;
}
static bool append_dependency(XrIoContext *io, XrLockedPackage *package, char *owned) {
    char **expanded = NULL; int capacity = package->dep_capacity;
    if (package->dep_count == capacity)
        expanded = grow_array(io, package->dependencies, package->dep_count, capacity, sizeof(*expanded), &capacity);
    if (!io_work(io, sizeof(owned))) { io_free(io, expanded); return false; }
    if (expanded) { io_free(io, package->dependencies); package->dependencies = expanded; package->dep_capacity = capacity; }
    package->dependencies[package->dep_count++] = owned; return true;
}
XR_FUNC XrOsIoStatus xr_lockfile_add_dependency_owned(XrLockfile *lock, const char *name, const char *dependency) {
    if (!lock || !io_policy_valid(&lock->policy) || !name || !dependency) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&lock->policy, XR_OS_IO_OK}; int index = find_index(&io, lock, name);
    if (io.status != XR_OS_IO_OK) return io.status;
    if (index < 0) return XR_OS_IO_NOT_FOUND;
    char *owned = copy_text(&io, dependency);
    if (!owned || !append_dependency(&io, &lock->packages[index], owned)) io_free(&io, owned);
    return io.status;
}
XR_FUNC XrOsIoStatus xr_lockfile_remove_owned(XrLockfile *lock, const char *name) {
    if (!lock || !io_policy_valid(&lock->policy) || !name) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&lock->policy, XR_OS_IO_OK}; int index = find_index(&io, lock, name);
    if (io.status != XR_OS_IO_OK) return io.status;
    if (index < 0) return XR_OS_IO_NOT_FOUND;
    size_t bytes = (size_t)(lock->package_count - index - 1) * sizeof(*lock->packages);
    if (io_work(&io, bytes + sizeof(lock->package_count))) {
        free_package(io.policy, &lock->packages[index]);
        memmove(&lock->packages[index], &lock->packages[index + 1], bytes); --lock->package_count;
    }
    return io.status;
}
static const char *skip_whitespace(XrIoContext *io, const char *text) {
    unsigned char c;
    while ((c = lock_character(io, text)) != 0 && isspace(c)) ++text;
    return text;
}
static const char *skip_comments(XrIoContext *io, const char *text) {
    while (io->status == XR_OS_IO_OK) {
        text = skip_whitespace(io, text);
        if (lock_character(io, text) != '#') break;
        unsigned char c;
        while ((c = lock_character(io, text)) != 0 && c != '\n') ++text;
    }
    return text;
}
static bool consume_literal(XrIoContext *io, const char **text, const char *literal, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (lock_character(io, *text + i) != (unsigned char)literal[i] || io->status != XR_OS_IO_OK) return false;
    *text += length; return true;
}
static char *parse_string(XrIoContext *io, const char **text) {
    const char *p = skip_whitespace(io, *text);
    if (lock_character(io, p) != '"') { io_status(io, XR_OS_IO_BAD_ARGUMENT); return NULL; }
    const char *start = ++p; unsigned char c;
    while ((c = lock_character(io, p)) != 0 && c != '"' && c != '\n') ++p;
    if (c != '"') { io_status(io, XR_OS_IO_BAD_ARGUMENT); return NULL; }
    size_t length = (size_t)(p - start); char *value = io_alloc(io, length + 1);
    if (value && io_copy(io, value, start, length) && io_work(io, 1)) value[length] = 0;
    *text = p + 1;
    if (io->status != XR_OS_IO_OK) { io_free(io, value); return NULL; }
    return value;
}
static void parse_array(XrIoContext *io, const char **text, XrLockedPackage *package) {
    const char *p = skip_whitespace(io, *text);
    if (lock_character(io, p) != '[') { io_status(io, XR_OS_IO_BAD_ARGUMENT); return; }
    ++p;
    free_dependencies(io->policy, package->dependencies, package->dep_count);
    package->dependencies = NULL; package->dep_count = 0; package->dep_capacity = 0;
    while (io->status == XR_OS_IO_OK) {
        p = skip_whitespace(io, p); unsigned char c = lock_character(io, p);
        if (c == ']') { *text = p + 1; return; }
        if (c != '"') { io_status(io, XR_OS_IO_BAD_ARGUMENT); return; }
        char *value = parse_string(io, &p);
        if (!value || !append_dependency(io, package, value)) { io_free(io, value); return; }
        p = skip_whitespace(io, p); c = lock_character(io, p);
        if (c == ',') ++p;
        else if (c != ']' && c != '"') { io_status(io, XR_OS_IO_BAD_ARGUMENT); return; }
    }
}
static bool parse_lockfile(XrIoContext *io, const char *text, XrLockfile *lock) {
    const char *p = text; char current[256];
    if (!io_work(io, 1)) return false;
    current[0] = 0;
    while (io->status == XR_OS_IO_OK) {
        p = skip_comments(io, p); unsigned char c = lock_character(io, p);
        if (!c) break;
        if (c == '[') {
            ++p;
            if (consume_literal(io, &p, "package.", 8)) {
                const char *start = p;
                while ((c = lock_character(io, p)) != 0 && c != ']' && c != '\n') ++p;
                if (c != ']') { io_status(io, XR_OS_IO_BAD_ARGUMENT); break; }
                size_t length = (size_t)(p - start);
                if (length >= sizeof(current)) { io_status(io, XR_OS_IO_BUDGET); break; }
                if (io_copy(io, current, start, length) && io_work(io, 1)) current[length] = 0;
                if (io->status == XR_OS_IO_OK) io_status(io, xr_lockfile_add_package_owned(lock, current, "0.0.0", "", ""));
                ++p;
            } else {
                while ((c = lock_character(io, p)) != 0 && c != ']') ++p;
                if (c == ']') ++p;
                if (io_work(io, 1)) current[0] = 0;
            }
            continue;
        }
        if (lock_character(io, current) && isalpha(c)) {
            const char *start = p;
            while ((c = lock_character(io, p)) != 0 && (isalnum(c) || c == '_')) ++p;
            size_t length = (size_t)(p - start); char field[64];
            if (length >= sizeof(field)) { io_status(io, XR_OS_IO_BUDGET); break; }
            if (io_copy(io, field, start, length) && io_work(io, 1)) field[length] = 0;
            p = skip_whitespace(io, p);
            if (lock_character(io, p) == '=') ++p;
            p = skip_whitespace(io, p);
            int index = find_index(io, lock, current);
            if (index >= 0) {
                XrLockedPackage *package = &lock->packages[index]; char **slot = NULL;
                if (equal_text(io, field, "version")) slot = &package->version;
                else if (equal_text(io, field, "resolved")) slot = &package->resolved;
                else if (equal_text(io, field, "checksum")) slot = &package->checksum;
                else if (equal_text(io, field, "dependencies")) parse_array(io, &p, package);
                if (slot && io->status == XR_OS_IO_OK) {
                    char *value = parse_string(io, &p);
                    if (value && io_work(io, sizeof(*slot))) { io_free(io, *slot); *slot = value; }
                    else io_free(io, value);
                }
            }
        }
        while ((c = lock_character(io, p)) != 0 && c != '\n') ++p;
        if (c == '\n') ++p;
    }
    return io->status == XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_lockfile_load_owned(const XrOsIoPolicy *policy, const char *path, XrLockfile **output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; uint8_t *bytes = NULL; size_t length = 0; char *text = NULL; XrLockfile *lock = NULL;
    io_status(&io, xr_os_io_read_regular_file(policy, path, SIZE_MAX - 1, &bytes, &length));
    if (io.status == XR_OS_IO_OK) text = io_alloc(&io, length + 1);
    if (text && io_copy(&io, text, bytes, length) && io_work(&io, 1)) text[length] = 0;
    io_free(&io, bytes);
    if (io.status == XR_OS_IO_OK) io_status(&io, xr_lockfile_new_owned(policy, &lock));
    if (io.status == XR_OS_IO_OK) parse_lockfile(&io, text, lock);
    io_free(&io, text);
    if (io_work(&io, sizeof(*output))) *output = lock;
    else xr_lockfile_free_owned(lock);
    return io.status;
}
typedef struct LockWriter { XrIoContext *io; char *bytes; size_t length, capacity; } LockWriter;
static bool append_span(LockWriter *writer, const char *text, size_t length) {
    if (length > SIZE_MAX - writer->length) return io_status(writer->io, XR_OS_IO_BUDGET);
    size_t need = writer->length + length;
    if (need > writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity : 256;
        while (capacity < need) {
            if (!io_work(writer->io, 1)) return false;
            if (capacity > SIZE_MAX / 2) { capacity = need; break; }
            capacity *= 2;
        }
        char *expanded = io_alloc(writer->io, capacity);
        if (!expanded) return false;
        if (!io_copy(writer->io, expanded, writer->bytes, writer->length)) { io_free(writer->io, expanded); return false; }
        io_free(writer->io, writer->bytes); writer->bytes = expanded; writer->capacity = capacity;
    }
    if (!io_copy(writer->io, writer->bytes + writer->length, text, length)) return false;
    writer->length += length; return true;
}
static bool append_text(LockWriter *writer, const char *text) {
    size_t length = 0;
    return io_length(writer->io, text, &length) && append_span(writer, text, length);
}
static bool append_integer(LockWriter *writer, int value) {
    uint32_t number = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    char reversed[10]; size_t length = 0;
    do {
        if (!io_work(writer->io, 1)) return false;
        reversed[length++] = (char)('0' + number % 10); number /= 10;
    } while (number);
    if (value < 0 && !append_span(writer, "-", 1)) return false;
    while (length) if (!append_span(writer, &reversed[--length], 1)) return false;
    return true;
}
static void serialize_lockfile(LockWriter *writer, const XrLockfile *lock) {
    append_text(writer, "# xray.lock - Auto-generated, do not edit manually\n# Format version: ");
    append_integer(writer, lock->version); append_text(writer, "\n\n");
    for (int i = 0; i < lock->package_count && io_work(writer->io, 1); ++i) {
        const XrLockedPackage *package = &lock->packages[i];
        append_text(writer, "[package."); append_text(writer, package->name); append_text(writer, "]\nversion = \"");
        append_text(writer, package->version ? package->version : "0.0.0"); append_text(writer, "\"\n");
        if (package->resolved && lock_character(writer->io, package->resolved)) {
            append_text(writer, "resolved = \""); append_text(writer, package->resolved); append_text(writer, "\"\n");
        }
        if (package->checksum && lock_character(writer->io, package->checksum)) {
            append_text(writer, "checksum = \""); append_text(writer, package->checksum); append_text(writer, "\"\n");
        }
        append_text(writer, "dependencies = [");
        for (int j = 0; j < package->dep_count && io_work(writer->io, 1); ++j) {
            if (j) append_text(writer, ", ");
            append_text(writer, "\""); append_text(writer, package->dependencies[j]); append_text(writer, "\"");
        }
        append_text(writer, "]\n\n");
    }
}
static atomic_uint_fast64_t temporary_sequence;
static char *temporary_path(XrIoContext *io, const char *path) {
    static const char hex[] = "0123456789abcdef";
    size_t length = 0;
    if (!io_length(io, path, &length)) return NULL;
    if (length > SIZE_MAX - 43) { io_status(io, XR_OS_IO_BUDGET); return NULL; }
    char *temporary = io_alloc(io, length + 43);
    if (temporary && io_copy(io, temporary, path, length) && io_copy(io, temporary + length, ".tmp-lock-", 10) && io_work(io, 1)) {
        int64_t pid = xr_proc_self_pid();
        if (pid <= 0) io_status(io, XR_OS_IO_IO);
        uint_fast64_t sequence = 0;
        if (io_work(io, 1)) sequence = atomic_load_explicit(&temporary_sequence, memory_order_relaxed);
        while (io->status == XR_OS_IO_OK) {
            if (sequence == UINT64_MAX) { io_status(io, XR_OS_IO_BUDGET); break; }
            if (!io_work(io, 1)) break;
            if (atomic_compare_exchange_weak_explicit(&temporary_sequence, &sequence, sequence + 1,
                    memory_order_relaxed, memory_order_relaxed)) break;
        }
        /* The name is not a capability or secret. CREATE_NEW alone admits
         * ownership; PID reuse and collisions never permit overwriting. */
        uint64_t parts[2] = {(uint64_t)pid, (uint64_t)sequence};
        for (size_t part = 0; part < 2 && io->status == XR_OS_IO_OK; ++part)
            for (size_t i = 0; i < 16 && io_work(io, 1); ++i)
                temporary[length + 10 + part * 16 + i] = hex[(parts[part] >> (60 - i * 4)) & 15];
        if (io_work(io, 1)) temporary[length + 42] = 0;
    }
    if (io->status != XR_OS_IO_OK) { io_free(io, temporary); return NULL; }
    return temporary;
}
XR_FUNC XrOsIoStatus xr_lockfile_save_owned(const XrOsIoPolicy *policy, const XrLockfile *lock, const char *path) {
    if (!xr_lockfile_uses_policy(lock, policy) || !path || !path[0]) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; LockWriter writer = {&io, NULL, 0, 0}; serialize_lockfile(&writer, lock);
    bool published = false;
    for (unsigned attempt = 0; attempt < 16 && io_work(&io, 1); ++attempt) {
        char *temporary = temporary_path(&io, path);
        if (!temporary) break;
        XrOsIoStatus status = xr_os_io_write_new_file_sync(policy, temporary, (const uint8_t *)writer.bytes, writer.length);
        if (status == XR_OS_IO_EXISTS) { io_free(&io, temporary); continue; }
        io_status(&io, status);
        if (io.status == XR_OS_IO_OK) {
            io_status(&io, xr_os_io_rename(policy, temporary, path));
            published = io.status == XR_OS_IO_OK;
            if (!published && io.status != XR_OS_IO_BUDGET && io.status != XR_OS_IO_OUT_OF_MEMORY)
                (void)xr_os_io_remove(policy, temporary);
        }
        io_free(&io, temporary); break;
    }
    if (!published && io.status == XR_OS_IO_OK) io_status(&io, XR_OS_IO_EXISTS);
    io_free(&io, writer.bytes); return io.status;
}
XR_FUNC XrOsIoStatus xr_lockfile_checksum_file_owned(const XrOsIoPolicy *policy, const char *path,
    char *output, size_t capacity) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (capacity < XR_LOCKFILE_CHECKSUM_CAPACITY) return XR_OS_IO_BUDGET;
    XrIoContext io = {policy, XR_OS_IO_OK}; uint8_t *bytes = NULL; size_t size = 0;
    io_status(&io, xr_os_io_read_regular_file(policy, path, SIZE_MAX, &bytes, &size));
    uint8_t digest[32]; char result[XR_LOCKFILE_CHECKSUM_CAPACITY]; static const char hex[] = "0123456789abcdef";
    if (size > UINT64_MAX - sizeof(digest)) io_status(&io, XR_OS_IO_BUDGET);
    if (io_work(&io, size + sizeof(digest))) xr_sha256(bytes, size, digest);
    io_free(&io, bytes);
    if (io_copy(&io, result, "sha256:", 7)) {
        for (size_t i = 0; i < sizeof(digest) && io_work(&io, 3); ++i) {
            result[7 + i * 2] = hex[digest[i] >> 4]; result[8 + i * 2] = hex[digest[i] & 15];
        }
        if (io_work(&io, 1)) result[71] = 0;
        io_copy(&io, output, result, sizeof(result));
    }
    return io.status;
}
XR_FUNC XrOsIoStatus xr_lockfile_verify_checksum_owned(const XrOsIoPolicy *policy, const char *path,
    const char *expected, bool *output) {
    if (!io_policy_valid(policy) || !path || !expected || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; char actual[XR_LOCKFILE_CHECKSUM_CAPACITY];
    io_status(&io, xr_lockfile_checksum_file_owned(policy, path, actual, sizeof(actual)));
    bool equal = io.status == XR_OS_IO_OK && equal_text(&io, actual, expected);
    if (io_work(&io, sizeof(*output))) *output = equal;
    return io.status;
}
