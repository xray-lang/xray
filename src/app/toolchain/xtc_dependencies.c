/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_dependencies.c - Closed native dependency report grammars
 *
 * KEY CONCEPT:
 *   The result owns observed text, not file authority or a complete search domain.
 */
#include "xtc_dependencies.h"
#include "base/xjson_cursor.h"
#include "shared/xr_utf8_core.h"
#include <string.h>

struct XrDependencyFacts {
    XrCompileResources *resources;
    char *bytes, *text;
    const char *target;
    XrDependencyRecord *records;
    uint32_t count, capacity;
};
typedef struct DependencyParser {
    XrDependencyFacts *facts;
    XrDependencyLimits limits;
    XrJsonCursor json;
    size_t text_used, text_capacity;
} DependencyParser;

static XrJsonCursorStatus dependency_resource(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_JSON_CURSOR_OK;
    case XR_COMPILE_RESOURCE_BUDGET: return XR_JSON_CURSOR_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_JSON_CURSOR_OUT_OF_MEMORY;
    default: return XR_JSON_CURSOR_INVALID;
    }
}
static XrJsonCursorStatus dependency_work(void *context, uint64_t units) {
    return dependency_resource(xr_compile_resources_work(context, units));
}
static XrXirTargetStatus dependency_status(XrJsonCursorStatus status) {
    switch (status) {
    case XR_JSON_CURSOR_OK: return XR_XIR_TARGET_OK;
    case XR_JSON_CURSOR_BUDGET: return XR_XIR_TARGET_BUDGET;
    case XR_JSON_CURSOR_OUT_OF_MEMORY: return XR_XIR_TARGET_OUT_OF_MEMORY;
    case XR_JSON_CURSOR_UNSUPPORTED: return XR_XIR_TARGET_UNSUPPORTED;
    default: return XR_XIR_TARGET_INVALID;
    }
}
static bool dependency_fail(DependencyParser *p, XrJsonCursorStatus status) {
    return xr_json_cursor_fail(&p->json, status);
}
static int dependency_read(void *context, const uint8_t *address, uint8_t *output) {
    DependencyParser *p = context;
    if (!xr_json_cursor_work(&p->json, 1)) return 0;
    *output = *address; return 1;
}
static bool dependency_utf8(DependencyParser *p, const char *text, size_t length) {
    XrUtf8ScanResult result;
    if (!xr_utf8_core_scan_strict_read((const uint8_t *)text, length,
        dependency_read, p, &result)) return false;
    return result.error == XR_UTF8_OK || dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
static bool dependency_separator(uint8_t byte) { return byte == '/' || byte == '\\'; }
static bool dependency_path(DependencyParser *p, const char *path) {
    size_t length = 0;
    if (!xr_json_cursor_length(&p->json, path, &length)) return false;
    if (!length) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    if (length > p->limits.path_bytes) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
    if (!dependency_utf8(p, path, length)) return false;
    /* Windows file paths cannot contain control bytes or a non-drive colon. */
    for (size_t i = 0; i < length; ++i) {
        if (!xr_json_cursor_work(&p->json, 1)) return false;
        uint8_t c = (uint8_t)path[i];
        if (c < 32 || c == '"' || (c == ':' && i != 1))
            return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    }
    if (!xr_json_cursor_work(&p->json, length < 3 ? length : 3)) return false;
    uint8_t first = (uint8_t)path[0], second = length > 1 ? (uint8_t)path[1] : 0;
    uint8_t third = length > 2 ? (uint8_t)path[2] : 0;
    if (length >= 3 && ((first >= 'a' && first <= 'z') ||
        (first >= 'A' && first <= 'Z')) && second == ':' && dependency_separator(third)) return true;
    if (length >= 5 && dependency_separator(first) && dependency_separator(second)) {
        size_t i = 2, start = i;
        for (; i < length; ++i) {
            if (!xr_json_cursor_work(&p->json, 1)) return false;
            if (dependency_separator((uint8_t)path[i])) break;
        }
        if (i == start || i == length) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        start = ++i;
        for (; i < length; ++i) {
            if (!xr_json_cursor_work(&p->json, 1)) return false;
            if (dependency_separator((uint8_t)path[i])) break;
        }
        if (i > start) return true;
    }
    return dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
static bool dependency_append(DependencyParser *p, const char *path, size_t offset,
    XrDependencyKind kind) {
    XrDependencyFacts *f = p->facts;
    if (!dependency_path(p, path)) return false;
    if (f->count == p->limits.records) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
    if (f->count == f->capacity) {
        uint32_t capacity = f->capacity > UINT32_MAX / 2 ? UINT32_MAX : f->capacity * 2;
        if (!capacity) capacity = 8;
        if (capacity > p->limits.records) capacity = p->limits.records;
        size_t bytes = (size_t)capacity * sizeof(*f->records);
        if (bytes / sizeof(*f->records) != capacity) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
        XrJsonCursorStatus status = dependency_resource(xr_compile_resources_resize(
            f->resources, (void **)&f->records, bytes));
        if (status != XR_JSON_CURSOR_OK) return dependency_fail(p, status);
        f->capacity = capacity;
    }
    if (!xr_json_cursor_work(&p->json, sizeof(*f->records))) return false;
    f->records[f->count++] = (XrDependencyRecord){path, offset, kind};
    return true;
}
static bool dependency_json_path(DependencyParser *p, XrDependencyKind kind) {
    const char *path = NULL;
    if (!xr_json_cursor_space(&p->json)) return false;
    size_t offset = (size_t)(p->json.cursor - p->json.begin);
    return xr_json_cursor_string(&p->json, p->limits.path_bytes, &path) &&
        dependency_append(p, path, offset, kind);
}
static bool dependency_json_includes(DependencyParser *p) {
    uint8_t token;
    if (!xr_json_cursor_take(&p->json, '[') || !xr_json_cursor_space(&p->json) ||
        !xr_json_cursor_peek(&p->json, &token)) return false;
    if (token == ']') return xr_json_cursor_take(&p->json, ']');
    for (;;) {
        if (!dependency_json_path(p, XR_DEPENDENCY_HEADER) ||
            !xr_json_cursor_space(&p->json) || !xr_json_cursor_byte(&p->json, &token)) return false;
        if (token == ']') return true;
        if (token != ',') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    }
}
static bool dependency_json_data(DependencyParser *p) {
    static const char *const keys[] = {"Source", "ProvidedModule", "Includes"};
    uint64_t seen = 0; bool more = true;
    if (!xr_json_cursor_take(&p->json, '{')) return false;
    while (more) {
        uint32_t key;
        if (!xr_json_cursor_key(&p->json, keys, 3, &seen, &key)) return false;
        if (key == 0 && !dependency_json_path(p, XR_DEPENDENCY_SOURCE)) return false;
        if (key == 1) {
            const char *module = NULL;
            if (!xr_json_cursor_string(&p->json, p->limits.path_bytes, &module)) return false;
            if (!xr_json_cursor_equal(&p->json, module, ""))
                return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        }
        if (key == 2 && !dependency_json_includes(p)) return false;
        if (!xr_json_cursor_member_end(&p->json, &more)) return false;
    }
    return seen == 7 || dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
static bool dependency_json(DependencyParser *p) {
    static const char *const keys[] = {"Version", "Data"};
    uint64_t seen = 0; bool more = true;
    if (!xr_json_cursor_take(&p->json, '{')) return false;
    while (more) {
        uint32_t key;
        if (!xr_json_cursor_key(&p->json, keys, 2, &seen, &key)) return false;
        if (key == 0) {
            const char *version = NULL;
            if (!xr_json_cursor_string(&p->json, 32, &version)) return false;
            if (!xr_json_cursor_equal(&p->json, version, "1.2"))
                return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        } else if (!dependency_json_data(p)) return false;
        if (!xr_json_cursor_member_end(&p->json, &more)) return false;
    }
    return (seen == 3 && xr_json_cursor_space(&p->json) && p->json.cursor == p->json.end) ||
        dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
static bool dependency_put(DependencyParser *p, uint8_t byte) {
    if (p->text_used == p->text_capacity) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
    if (!xr_json_cursor_work(&p->json, 1)) return false;
    p->facts->text[p->text_used++] = (char)byte; return true;
}
static bool dependency_look(DependencyParser *p, size_t ahead, uint8_t *byte) {
    if ((size_t)(p->json.end - p->json.cursor) <= ahead)
        return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    if (!xr_json_cursor_work(&p->json, 1)) return false;
    *byte = (uint8_t)p->json.cursor[ahead]; return true;
}
static bool dependency_make_gap(DependencyParser *p, bool *ended) {
    while (p->json.cursor != p->json.end) {
        uint8_t c;
        if (!dependency_look(p, 0, &c)) return false;
        if (c == ' ') { ++p->json.cursor; continue; }
        if (c == '\\') {
            if (!dependency_look(p, 1, &c)) return false;
            if (c != '\r') break;
            if (*ended) return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
            if (!dependency_look(p, 2, &c)) return false;
            if (c != '\n') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            p->json.cursor += 3; continue;
        }
        if (c == '\r') {
            if (!dependency_look(p, 1, &c)) return false;
            if (c != '\n') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            p->json.cursor += 2; *ended = true; continue;
        }
        if (c == '\t' || c == '\n') return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        break;
    }
    return true;
}
static bool dependency_make_word(DependencyParser *p, bool target, const char **output) {
    size_t begin = p->text_used, count = 0;
    while (p->json.cursor != p->json.end) {
        uint8_t c;
        if (!dependency_look(p, 0, &c)) return false;
        if (c == ' ' || c == '\r' || c == '\n') break;
        if (c == ':' && target && count != 1) break;
        if (c == '$' || c == '#' || c == '\t' || c == ';' || c == '%' || c == '*' ||
            c == '?' || c == '[' || c == ']' || c == '|' || (target && c == '&'))
            return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        if (c < 32) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        if (c == '\\' && p->json.end - p->json.cursor > 1) {
            uint8_t next;
            if (!dependency_look(p, 1, &next)) return false;
            if (next == '\r' || next == '\n') break;
            if (next == ' ') { ++p->json.cursor; c = next; }
        }
        if (++count > p->limits.path_bytes) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
        ++p->json.cursor;
        if (!dependency_put(p, c)) return false;
    }
    if (!count) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    if (!dependency_put(p, 0)) return false;
    *output = p->facts->text + begin; return true;
}
static bool dependency_make(DependencyParser *p) {
    bool ended = false; const char *target;
    uint8_t colon;
    if (!dependency_make_word(p, true, &target) || !dependency_path(p, target) ||
        !xr_json_cursor_byte(&p->json, &colon)) return false;
    if (colon != ':') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    p->facts->target = target;
    while (p->json.cursor != p->json.end) {
        if (!dependency_make_gap(p, &ended)) return false;
        if (p->json.cursor == p->json.end) break;
        if (ended) return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        const char *path;
        size_t offset = (size_t)(p->json.cursor - p->json.begin);
        if (!dependency_make_word(p, false, &path) || !dependency_append(p, path, offset,
            p->facts->count ? XR_DEPENDENCY_HEADER : XR_DEPENDENCY_SOURCE)) return false;
    }
    return p->facts->count != 0 || dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
static bool dependency_prefix(DependencyParser *p, const char *line, const char *prefix,
    size_t *consumed) {
    size_t i = 0;
    for (;;) {
        if (!xr_json_cursor_work(&p->json, 1)) return false;
        char expected = prefix[i];
        if (!expected) { *consumed = i; return true; }
        if (!xr_json_cursor_work(&p->json, 1)) return false;
        if (line[i] != expected) return false;
        ++i;
    }
}
static bool dependency_libraries(DependencyParser *p) {
    bool searching = false; uint32_t blocks = 0, block_records = 0;
    while (p->json.cursor != p->json.end) {
        size_t offset = (size_t)(p->json.cursor - p->json.begin), length = 0;
        char *line = p->json.cursor;
        while (p->json.cursor != p->json.end) {
            uint8_t c;
            if (!xr_json_cursor_byte(&p->json, &c)) return false;
            if (c == '\r') {
                if (!xr_json_cursor_byte(&p->json, &c)) return false;
                if (c != '\n') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
                break;
            }
            if (!c || c == '\n') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            ++length;
        }
        if (!xr_json_cursor_work(&p->json, 1)) return false;
        line[length] = 0;
        if (!length) continue;
        if (xr_json_cursor_equal(&p->json, line, "正在搜索库")) {
            if (searching) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            searching = true; block_records = 0; continue;
        }
        if (xr_json_cursor_equal(&p->json, line, "已完成库搜索")) {
            if (!searching || !block_records) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            searching = false; ++blocks; continue;
        }
        size_t prefix;
        if (!dependency_prefix(p, line, "    正在搜索 ", &prefix))
            return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        if (!searching || length <= prefix || !xr_json_cursor_work(&p->json, 1))
            return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        if (line[length - 1] != ':') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        if (!xr_json_cursor_work(&p->json, 1)) return false;
        line[length - 1] = 0;
        if (!dependency_append(p, line + prefix, offset + prefix, XR_DEPENDENCY_LIBRARY_SEARCH)) return false;
        ++block_records;
    }
    return (!searching && blocks) || dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
static bool dependency_arguments(DependencyParser *p, const XrDependencyInput *input) {
    for (uint32_t i = 0; i < input->argc; ++i) {
        if (!xr_json_cursor_work(&p->json, 1)) return false;
        const char *arg = input->argv[i];
        if (!arg) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        size_t length = 0;
        for (;;) {
            if (!xr_json_cursor_work(&p->json, 1)) return false;
            uint8_t byte = (uint8_t)arg[length];
            if (byte == '@') return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
            if (!byte) break;
            ++length;
        }
        if ((!i && !length) || !dependency_utf8(p, arg, length))
            return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    }
    return true;
}
XR_FUNC XrXirTargetStatus xtc_dependencies_parse(XrCompileResources *resources,
    const XrDependencyInput *input, XrDependencyLimits limits, XrDependencyFacts **output) {
    if (!resources || !input || !input->bytes || !input->length || !input->argv ||
        !input->argc || !output || *output || !limits.frame_bytes || !limits.path_bytes || !limits.records)
        return XR_XIR_TARGET_INVALID;
    if (input->format < XR_DEPENDENCY_MSVC_SOURCE_1_2 || input->format > XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN)
        return XR_XIR_TARGET_UNSUPPORTED;
    if (input->length > limits.frame_bytes || input->length > PTRDIFF_MAX) return XR_XIR_TARGET_BUDGET;
    DependencyParser p = {0}; p.limits = limits;
    p.json = xr_json_cursor_make(NULL, 0, resources, dependency_work);
    if (!dependency_arguments(&p, input)) return dependency_status(p.json.status);
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, 1,
        sizeof(*p.facts), (void **)&p.facts);
    if (status != XR_COMPILE_RESOURCE_OK) return dependency_status(dependency_resource(status));
    p.facts->resources = resources;
    status = xr_compile_resources_alloc(resources, input->length + 1, (void **)&p.facts->bytes);
    if (status != XR_COMPILE_RESOURCE_OK) dependency_fail(&p, dependency_resource(status));
    if (p.json.status == XR_JSON_CURSOR_OK && xr_json_cursor_work(&p.json, input->length + 1)) {
        memcpy(p.facts->bytes, input->bytes, input->length); p.facts->bytes[input->length] = 0;
        p.json.begin = p.json.cursor = p.facts->bytes; p.json.end = p.json.begin + input->length;
        if (input->format == XR_DEPENDENCY_WINDOWS_MAKE) {
            p.text_capacity = input->length + 1;
            status = xr_compile_resources_alloc(resources, p.text_capacity, (void **)&p.facts->text);
            if (status != XR_COMPILE_RESOURCE_OK) dependency_fail(&p, dependency_resource(status));
        }
        if (p.json.status == XR_JSON_CURSOR_OK) {
            if (input->format == XR_DEPENDENCY_MSVC_SOURCE_1_2) dependency_json(&p);
            else if (input->format == XR_DEPENDENCY_WINDOWS_MAKE) dependency_make(&p);
            else dependency_libraries(&p);
        }
    }
    if (p.json.status != XR_JSON_CURSOR_OK) {
        XrXirTargetStatus result = dependency_status(p.json.status);
        xtc_dependencies_free(p.facts); return result;
    }
    *output = p.facts; return XR_XIR_TARGET_OK;
}
XR_FUNC XrCompileResources *xtc_dependencies_resources(const XrDependencyFacts *facts) {
    return facts ? facts->resources : NULL;
}
XR_FUNC XrXirTargetStatus xtc_dependencies_status(const XrDependencyFacts *facts) {
    return facts ? XR_XIR_TARGET_OK : XR_XIR_TARGET_INVALID;
}
XR_FUNC uint32_t xtc_dependencies_count(const XrDependencyFacts *facts) { return facts ? facts->count : 0; }
XR_FUNC const XrDependencyRecord *xtc_dependencies_record(const XrDependencyFacts *facts, uint32_t index) {
    return facts && index < facts->count ? &facts->records[index] : NULL;
}
XR_FUNC const char *xtc_dependencies_target(const XrDependencyFacts *facts) { return facts ? facts->target : NULL; }
XR_FUNC XrXirTargetStatus xtc_dependencies_response_facts(const XrDependencyFacts *facts,
    XrDependencyResponseFacts *output) {
    if (!facts || !output) return XR_XIR_TARGET_INVALID;
    *output = XR_DEPENDENCY_NO_REFERENCE_IN_CAPTURED_ARGV; return XR_XIR_TARGET_OK;
}
XR_FUNC void xtc_dependencies_free(XrDependencyFacts *facts) {
    if (!facts) return;
    xr_compile_resources_free(facts->records); xr_compile_resources_free(facts->text);
    xr_compile_resources_free(facts->bytes); xr_compile_resources_free(facts);
}
