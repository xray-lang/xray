/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemver.c - One policy-bearing version and constraint parser
 */
#include "xsemver.h"
#include "../base/xio_policy.inc.h"
#include <ctype.h>
#include <limits.h>

static unsigned char character(XrIoContext *io, const char *text) {
    return io_work(io, 1) ? (unsigned char)*text : 0;
}
static const char *skip_whitespace(XrIoContext *io, const char *text) {
    unsigned char c;
    while ((c = character(io, text)) != 0 && isspace(c)) ++text;
    return text;
}
static bool parse_number(XrIoContext *io, const char **text, int *output) {
    const char *p = *text; unsigned char c = character(io, p);
    if (!isdigit(c)) return false;
    if (c == '0' && isdigit(character(io, p + 1))) return false;
    uint32_t value = 0;
    while (isdigit(c)) {
        unsigned digit = c - '0';
        if (value > ((uint32_t)INT32_MAX - digit) / 10) return false;
        value = value * 10 + digit; c = character(io, ++p);
    }
    *text = p; *output = (int)value; return io->status == XR_OS_IO_OK;
}
static char *parse_identifier(XrIoContext *io, const char **text) {
    const char *start = *text; unsigned char c;
    while ((c = character(io, *text)) != 0 && c != '+' && (isalnum(c) || c == '.' || c == '-')) ++*text;
    if (*text == start || io->status != XR_OS_IO_OK) return NULL;
    size_t length = (size_t)(*text - start);
    if (length == SIZE_MAX) { io_status(io, XR_OS_IO_BUDGET); return NULL; }
    char *result = io_alloc(io, length + 1);
    if (result && io_copy(io, result, start, length) && io_work(io, 1)) result[length] = 0;
    if (io->status != XR_OS_IO_OK) { io_free(io, result); return NULL; }
    return result;
}
XR_FUNC void xr_semver_free_owned(XrSemVer *version) {
    if (!version) return;
    XrOsIoPolicy policy = version->policy;
    if (version->prerelease) policy.free(policy.context, version->prerelease);
    if (version->build) policy.free(policy.context, version->build);
    memset(version, 0, sizeof(*version));
}
/* The existing accepted grammar includes v/V, abbreviated numeric versions,
 * optional suffixes, and whitespace. Validation and construction share it. */
static bool parse_version(XrIoContext *io, const char *text, XrSemVer *version) {
    if (!io_clear(io, version, sizeof(*version))) return false;
    version->policy = *io->policy;
    const char *p = skip_whitespace(io, text); unsigned char c = character(io, p);
    if (c == 'v' || c == 'V') ++p;
    if (!parse_number(io, &p, &version->major)) return false;
    if (character(io, p) != '.') return io->status == XR_OS_IO_OK;
    ++p;
    if (!parse_number(io, &p, &version->minor)) return false;
    if (character(io, p) == '.') {
        ++p;
        if (!parse_number(io, &p, &version->patch)) return false;
    }
    if (character(io, p) == '-') { ++p; version->prerelease = parse_identifier(io, &p); }
    if (character(io, p) == '+') { ++p; version->build = parse_identifier(io, &p); }
    p = skip_whitespace(io, p);
    return character(io, p) == 0 && io->status == XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_semver_parse_owned(const XrOsIoPolicy *policy, const char *text, XrSemVer *output) {
    if (!io_policy_valid(policy) || !text || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; XrSemVer pending = {0};
    bool valid = parse_version(&io, text, &pending);
    if (valid && io_copy(&io, output, &pending, sizeof(pending))) return XR_OS_IO_OK;
    xr_semver_free_owned(&pending);
    return io.status != XR_OS_IO_OK ? io.status : XR_OS_IO_BAD_ARGUMENT;
}
XR_FUNC XrOsIoStatus xr_semver_is_valid_owned(const XrOsIoPolicy *policy, const char *text, bool *output) {
    if (!io_policy_valid(policy) || !text || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; XrSemVer pending = {0};
    bool valid = parse_version(&io, text, &pending); xr_semver_free_owned(&pending);
    if (io_work(&io, sizeof(*output))) *output = valid;
    return io.status;
}
static size_t segment_length(XrIoContext *io, const char *text) {
    size_t length = 0; unsigned char c;
    while ((c = character(io, text + length)) != 0 && c != '.') ++length;
    return length;
}
static bool numeric_segment(XrIoContext *io, const char *text, size_t length) {
    for (size_t i = 0; i < length; ++i) if (!isdigit(character(io, text + i))) return false;
    return io->status == XR_OS_IO_OK;
}
static int compare_segment(XrIoContext *io, const char *a, size_t al, const char *b, size_t bl) {
    bool an = numeric_segment(io, a, al), bn = numeric_segment(io, b, bl);
    if (an && bn) {
        /* Arbitrarily long numeric identifiers need no machine-integer parse.
         * Leading zeros retain the existing numeric-equality behavior. */
        while (al && character(io, a) == '0') { ++a; --al; }
        while (bl && character(io, b) == '0') { ++b; --bl; }
        if (!io_work(io, 1)) return 0;
        if (al != bl) return al < bl ? -1 : 1;
    }
    size_t common = al < bl ? al : bl;
    for (size_t i = 0; i < common; ++i) {
        unsigned char av = character(io, a + i), bv = character(io, b + i);
        if (!io_work(io, 1)) return 0;
        if (av != bv) return av < bv ? -1 : 1;
    }
    if (!io_work(io, 1)) return 0;
    return (al > bl) - (al < bl);
}
static int compare_prerelease(XrIoContext *io, const char *a, const char *b) {
    if (!io_work(io, 1)) return 0;
    if (!a || !b) return !a && !b ? 0 : !a ? 1 : -1;
    while (io->status == XR_OS_IO_OK) {
        unsigned char ac = character(io, a), bc = character(io, b);
        if (!ac || !bc) return (ac != 0) - (bc != 0);
        size_t al = segment_length(io, a), bl = segment_length(io, b);
        int compared = compare_segment(io, a, al, b, bl);
        if (compared || io->status != XR_OS_IO_OK) return compared;
        a += al; b += bl;
        if (character(io, a) == '.') ++a;
        if (character(io, b) == '.') ++b;
    }
    return 0;
}
static int compare_version(XrIoContext *io, const XrSemVer *a, const XrSemVer *b) {
    if (!io_work(io, 1)) return 0;
    if (a->major != b->major) return (a->major > b->major) - (a->major < b->major);
    if (!io_work(io, 1)) return 0;
    if (a->minor != b->minor) return (a->minor > b->minor) - (a->minor < b->minor);
    if (!io_work(io, 1)) return 0;
    if (a->patch != b->patch) return (a->patch > b->patch) - (a->patch < b->patch);
    return compare_prerelease(io, a->prerelease, b->prerelease);
}
XR_FUNC XrOsIoStatus xr_semver_compare_owned(const XrOsIoPolicy *policy, const XrSemVer *a,
    const XrSemVer *b, int *output) {
    if (!io_policy_valid(policy) || !a || !b || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int result = compare_version(&io, a, b);
    if (io_work(&io, sizeof(*output))) *output = result;
    return io.status;
}
XR_FUNC void xr_constraint_free_owned(XrVersionConstraint *constraint) {
    if (constraint) xr_semver_free_owned(&constraint->version);
}
XR_FUNC XrOsIoStatus xr_constraint_parse_owned(const XrOsIoPolicy *policy, const char *text,
    XrVersionConstraint *output) {
    if (!io_policy_valid(policy) || !text || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; XrVersionConstraint pending = {0};
    const char *p = skip_whitespace(&io, text); unsigned char c = character(&io, p);
    pending.op = SEMVER_OP_EQ;
    if (c == '^') { pending.op = SEMVER_OP_CARET; ++p; }
    else if (c == '~') { pending.op = SEMVER_OP_TILDE; ++p; }
    else if (c == '>' || c == '<') {
        bool equal = character(&io, p + 1) == '=';
        pending.op = c == '>' ? (equal ? SEMVER_OP_GE : SEMVER_OP_GT) : (equal ? SEMVER_OP_LE : SEMVER_OP_LT);
        p += equal ? 2 : 1;
    } else if (c == '=') ++p;
    else if (c == '*') { pending.op = SEMVER_OP_ANY; ++p; }
    p = skip_whitespace(&io, p);
    bool valid = pending.op == SEMVER_OP_ANY ? character(&io, p) == 0 : parse_version(&io, p, &pending.version);
    if (valid && io_copy(&io, output, &pending, sizeof(pending))) return XR_OS_IO_OK;
    xr_constraint_free_owned(&pending);
    return io.status == XR_OS_IO_OK ? XR_OS_IO_BAD_ARGUMENT : io.status;
}
static bool matches_constraint(XrIoContext *io, const XrSemVer *version, const XrVersionConstraint *constraint) {
    int comparison = compare_version(io, version, &constraint->version);
    if (!io_work(io, 1)) return false;
    const XrSemVer *base = &constraint->version;
    switch (constraint->op) {
    case SEMVER_OP_ANY: return true;
    case SEMVER_OP_EQ: return comparison == 0;
    case SEMVER_OP_GT: return comparison > 0;
    case SEMVER_OP_GE: return comparison >= 0;
    case SEMVER_OP_LT: return comparison < 0;
    case SEMVER_OP_LE: return comparison <= 0;
    case SEMVER_OP_CARET:
        if (comparison < 0) return false;
        if (!base->major) return !base->minor ? !version->major && !version->minor && version->patch == base->patch :
            !version->major && version->minor == base->minor;
        return version->major == base->major;
    case SEMVER_OP_TILDE: return comparison >= 0 && version->major == base->major && version->minor == base->minor;
    }
    io_status(io, XR_OS_IO_BAD_ARGUMENT); return false;
}
XR_FUNC XrOsIoStatus xr_constraint_matches_owned(const XrOsIoPolicy *policy, const XrSemVer *version,
    const XrVersionConstraint *constraint, bool *output) {
    if (!io_policy_valid(policy) || !version || !constraint || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; bool result = matches_constraint(&io, version, constraint);
    if (io_work(&io, sizeof(*output))) *output = result;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_semver_select_best_owned(const XrOsIoPolicy *policy, const XrSemVer *versions,
    int count, const XrVersionConstraint *constraint, int *output) {
    if (!io_policy_valid(policy) || count < 0 || (!versions && count) || !constraint || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int best = -1;
    for (int i = 0; i < count && io_work(&io, 1); ++i) {
        if (matches_constraint(&io, &versions[i], constraint) &&
            (best < 0 || compare_version(&io, &versions[i], &versions[best]) > 0)) best = i;
    }
    if (io_work(&io, sizeof(*output))) *output = best;
    return io.status;
}
static size_t integer_text(XrIoContext *io, int value, char output[12]) {
    uint32_t magnitude = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    char reversed[10]; size_t count = 0, written = 0;
    do {
        if (!io_work(io, 1)) return 0;
        reversed[count++] = (char)('0' + magnitude % 10); magnitude /= 10;
    } while (magnitude);
    if (value < 0 && io_work(io, 1)) output[written++] = '-';
    while (count && io_work(io, 2)) output[written++] = reversed[--count];
    return written;
}
static XrOsIoStatus format_version(const XrOsIoPolicy *policy, const XrSemVer *version, const char *prefix,
    bool any, char *output, size_t capacity, size_t *written) {
    if (!io_policy_valid(policy) || !version || !output || !capacity || !written) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; size_t pre = 0, build = 0, pl = 0;
    if (!io_length(&io, prefix, &pl)) return io.status;
    char major[12], minor[12], patch[12]; size_t ml = 0, nl = 0, kl = 0;
    if (!any) {
        ml = integer_text(&io, version->major, major); nl = integer_text(&io, version->minor, minor);
        kl = integer_text(&io, version->patch, patch);
        if (version->prerelease) io_length(&io, version->prerelease, &pre);
        if (version->build) io_length(&io, version->build, &build);
    }
    if (pre > SIZE_MAX - build || pre + build > SIZE_MAX - 48) return XR_OS_IO_BUDGET;
    size_t size = any ? 1 : pl + ml + nl + kl + 2 + pre + build + (version->prerelease ? 1 : 0) + (version->build ? 1 : 0);
    if (io.status != XR_OS_IO_OK) return io.status;
    if (size >= capacity) return XR_OS_IO_BUDGET;
    char *buffer = io_alloc(&io, size + 1); size_t offset = 0;
#define APPEND(text, length) do { if (io_copy(&io, buffer + offset, (text), (length))) offset += (length); } while (0)
    if (buffer) {
        if (any) APPEND("*", 1);
        else {
            APPEND(prefix, pl); APPEND(major, ml); APPEND(".", 1); APPEND(minor, nl); APPEND(".", 1); APPEND(patch, kl);
            if (version->prerelease) { APPEND("-", 1); APPEND(version->prerelease, pre); }
            if (version->build) { APPEND("+", 1); APPEND(version->build, build); }
        }
        APPEND("", 1);
        if (io_work(&io, size + 1 + sizeof(*written))) { memcpy(output, buffer, size + 1); *written = size; }
    }
#undef APPEND
    io_free(&io, buffer); return io.status;
}
XR_FUNC XrOsIoStatus xr_semver_to_string_owned(const XrOsIoPolicy *policy, const XrSemVer *version,
    char *buffer, size_t capacity, size_t *written) {
    return format_version(policy, version, "", false, buffer, capacity, written);
}
XR_FUNC XrOsIoStatus xr_constraint_to_string_owned(const XrOsIoPolicy *policy, const XrVersionConstraint *constraint,
    char *buffer, size_t capacity, size_t *written) {
    static const char *const prefixes[] = {"", ">", ">=", "<", "<=", "^", "~", "*"};
    if (!constraint || constraint->op < SEMVER_OP_EQ || constraint->op > SEMVER_OP_ANY) return XR_OS_IO_BAD_ARGUMENT;
    return format_version(policy, &constraint->version, prefixes[constraint->op], constraint->op == SEMVER_OP_ANY,
        buffer, capacity, written);
}
