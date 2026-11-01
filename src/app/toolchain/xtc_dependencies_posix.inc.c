/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_dependencies_posix.inc.c - POSIX compiler and linker report grammars
 *
 * KEY CONCEPT:
 *   Only the closed spellings that cc -MD, GNU ld/LLD --dependency-file and
 *   ld64 -dependency_info emit for absolute paths are accepted; anything a
 *   make interpreter would expand is unsupported rather than guessed.
 */

/* POSIX file paths are absolute UTF-8 without control bytes. Backslashes are
 * legal names but cannot be told apart from make escapes, so they fail. */
static bool dependency_posix_path(DependencyParser *p, const char *path) {
    size_t length = 0;
    if (!xr_json_cursor_length(&p->json, path, &length)) return false;
    if (length < 2 || path[0] != '/') return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    if (length > p->limits.path_bytes) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
    if (!dependency_utf8(p, path, length)) return false;
    for (size_t i = 0; i < length; ++i) {

        uint8_t c = (uint8_t)path[i];
        if (c < 32 || c == 127) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        if (c == '\\') return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
    }
    return true;
}
static bool dependency_posix_append(DependencyParser *p, const char *path, size_t offset,
    XrDependencyKind kind) {
    return dependency_posix_path(p, path) && dependency_store(p, path, offset, kind);
}
/* Spaces and backslash-newline continuations separate words. A bare newline
 * ends the rule and is consumed. */
static bool dependency_posix_gap(DependencyParser *p, bool *ended) {
    *ended = false;
    while (p->json.cursor != p->json.end) {
        uint8_t c = 0;
        if (!dependency_look(p, 0, &c)) return false;
        if (c == ' ') { ++p->json.cursor; continue; }
        if (c == '\\') {
            if (!dependency_look(p, 1, &c)) return false;
            if (c != '\n') break;
            p->json.cursor += 2; continue;
        }
        if (c == '\n') { ++p->json.cursor; *ended = true; return true; }
        if (c == '\t' || c == '\r') return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        break;
    }
    return true;
}
/* GCC escapes a space or '#' with a backslash and '$' as "$$"; GNU ld writes
 * names verbatim. Make wildcards, variables and quoting are unsupported. */
static bool dependency_posix_word(DependencyParser *p, bool rule, const char **output) {
    size_t begin = p->text_used, count = 0;
    while (p->json.cursor != p->json.end) {
        uint8_t c = 0;
        if (!dependency_look(p, 0, &c)) return false;
        if (c == ' ' || c == '\n' || (rule && c == ':')) break;
        if (c == '\\') {
            uint8_t next = 0;
            if (!dependency_look(p, 1, &next)) return false;
            if (next == '\n') break;
            if (next != ' ' && next != '#') return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
            ++p->json.cursor; c = next;
        } else if (c == '$') {
            uint8_t next = 0;
            if (!dependency_look(p, 1, &next)) return false;
            if (next != '$') return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
            ++p->json.cursor;
        } else if (c == '#' || c == ':' || c == ';' || c == '%' || c == '*' || c == '?' ||
            c == '[' || c == ']' || c == '|' || c == '\t' || c == '\r' || c == '"' || c == '\'') {
            return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
        }
        if (c < 32) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        if (++count > p->limits.path_bytes) return dependency_fail(p, XR_JSON_CURSOR_BUDGET);
        ++p->json.cursor;
        if (!dependency_put(p, c)) return false;
    }
    if (!count) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    if (!dependency_put(p, 0)) return false;
    *output = p->facts->text + begin; return true;
}
static bool dependency_posix_take(DependencyParser *p, uint8_t expected) {
    uint8_t c;
    if (!xr_json_cursor_byte(&p->json, &c)) return false;
    return c == expected || dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
/* The GNU ld/LLD tail repeats every prerequisite as an empty rule, in order. */
static bool dependency_posix_phony(DependencyParser *p) {
    for (uint32_t i = 0; i < p->facts->count; ++i) {
        const char *name = NULL;
        if (!dependency_posix_take(p, '\n') || !dependency_posix_word(p, true, &name) ||
            !dependency_posix_take(p, ':') || !dependency_posix_take(p, '\n')) return false;
        if (!xr_json_cursor_equal(&p->json, name, p->facts->records[i].path))
            return p->json.status == XR_JSON_CURSOR_OK ? dependency_fail(p, XR_JSON_CURSOR_INVALID) : false;
    }
    return true;
}
static bool dependency_posix_make(DependencyParser *p, XrDependencyKind kind) {
    const char *target = NULL; bool ended = false;
    if (!dependency_posix_word(p, true, &target) || !dependency_posix_path(p, target) ||
        !dependency_posix_take(p, ':')) return false;
    p->facts->target = target;
    while (p->json.cursor != p->json.end) {
        if (!dependency_posix_gap(p, &ended)) return false;
        if (ended || p->json.cursor == p->json.end) break;
        const char *path = NULL;
        size_t offset = (size_t)(p->json.cursor - p->json.begin);
        if (!dependency_posix_word(p, false, &path) || !dependency_posix_append(p, path, offset, kind)) return false;
    }
    if (!p->facts->count) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
    if (kind == XR_DEPENDENCY_LINK_INPUT && !dependency_posix_phony(p)) return false;
    return p->json.cursor == p->json.end || dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
}
/* ld64 records: opcode byte, NUL-terminated UTF-8 text. 0x00 linker version
 * (first and only once), 0x10 used input, 0x11 unresolved search candidate,
 * 0x40 output. Other opcodes are unsupported. */
static bool dependency_darwin_info(DependencyParser *p) {
    bool versioned = false;
    while (p->json.cursor != p->json.end) {
        uint8_t opcode;
        if (!xr_json_cursor_byte(&p->json, &opcode)) return false;
        size_t offset = (size_t)(p->json.cursor - p->json.begin);
        const char *text = p->json.cursor;
        while (p->json.cursor != p->json.end && *p->json.cursor) ++p->json.cursor;
        if (p->json.cursor == p->json.end) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        size_t length = (size_t)(p->json.cursor - text);
        ++p->json.cursor;
        if (!dependency_utf8(p, text, length)) return false;
        if (opcode == 0x00) {
            /* ld64 writes its "@(#)PROGRAM:ld PROJECT:ld-N" banner with a
             * trailing newline; no other control byte is accepted. */
            if (versioned || p->facts->count || p->facts->target) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            for (size_t i = 0; i < length; ++i) {

                uint8_t c = (uint8_t)text[i];
                if (c < 32 && !(c == '\n' && i + 1 == length)) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
            }
            versioned = true; continue;
        }
        if (!versioned) return dependency_fail(p, XR_JSON_CURSOR_INVALID);
        if (opcode == 0x10 || opcode == 0x11) {
            if (!dependency_posix_append(p, text, offset,
                opcode == 0x10 ? XR_DEPENDENCY_LINK_INPUT : XR_DEPENDENCY_LIBRARY_SEARCH)) return false;
        } else if (opcode == 0x40) {
            if (p->facts->target || !dependency_posix_path(p, text)) return p->facts->target ?
                dependency_fail(p, XR_JSON_CURSOR_INVALID) : false;
            p->facts->target = text;
        } else return dependency_fail(p, XR_JSON_CURSOR_UNSUPPORTED);
    }
    return (versioned && p->facts->target && p->facts->count) || dependency_fail(p, XR_JSON_CURSOR_INVALID);
}
