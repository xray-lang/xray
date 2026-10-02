/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_pe_version.c - Bounded, allocation-free fixed PE version decoding
 */
#include "xtc_xir_pe_version.h"
#include <string.h>

typedef struct PeVersionReader {
    XrCompileResources *resources;
    const uint8_t *bytes;
    size_t length, sections, resource;
    uint32_t resource_size;
    uint16_t section_count;
    XtcXirPeVersionStatus status;
    bool found;
    uint32_t fixed[13];
} PeVersionReader;

static bool pe_fail(PeVersionReader *reader, XtcXirPeVersionStatus status) {
    if (reader->status == XTC_XIR_PE_VERSION_OK) reader->status = status;
    return false;
}
static bool pe_work(PeVersionReader *reader, uint64_t work) {
    XrCompileResourceStatus status = xr_compile_resources_work(reader->resources, work);
    return status == XR_COMPILE_RESOURCE_OK || pe_fail(reader,
        status == XR_COMPILE_RESOURCE_BUDGET ? XTC_XIR_PE_VERSION_BUDGET : XTC_XIR_PE_VERSION_BAD_ARGUMENT);
}
static bool pe_range(PeVersionReader *reader, size_t offset, size_t length) {
    return (offset <= reader->length && length <= reader->length - offset) ||
        pe_fail(reader, XTC_XIR_PE_VERSION_TRUNCATED);
}
static bool pe_read(PeVersionReader *reader, size_t offset, unsigned width, uint32_t *output) {
    if (!pe_range(reader, offset, width) || !pe_work(reader, width)) return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= (uint32_t)reader->bytes[offset + i] << (8 * i);
    *output = value;
    return true;
}
/* RVA translation never interprets zero-filled virtual bytes as file bytes.
 * Check every candidate rather than choosing the first overlapping section. */
static bool pe_rva(PeVersionReader *reader, uint32_t rva, uint32_t length, size_t *output) {
    if (!length || length > UINT32_MAX - rva) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    bool found = false;
    size_t result = 0;
    for (uint32_t i = 0; i < reader->section_count; ++i) {
        size_t entry = reader->sections + (size_t)i * 40;
        uint32_t virtual_size, address, raw_size, raw;
        if (!pe_read(reader, entry + 8, 4, &virtual_size) || !pe_read(reader, entry + 12, 4, &address) ||
            !pe_read(reader, entry + 16, 4, &raw_size) ||
            !pe_read(reader, entry + 20, 4, &raw)) return false;
        uint32_t mapped = virtual_size > raw_size ? virtual_size : raw_size;
        if (!mapped) continue;
        if (rva >= address + mapped || address >= rva + length) continue;
        if (rva < address || rva - address > raw_size || length > raw_size - (rva - address))
            return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
        if (found) return pe_fail(reader, XTC_XIR_PE_VERSION_AMBIGUOUS);
        if ((size_t)raw > reader->length || (size_t)(rva - address) > reader->length - raw)
            return pe_fail(reader, XTC_XIR_PE_VERSION_TRUNCATED);
        result = (size_t)raw + (rva - address);
        if (!pe_range(reader, result, length)) return false;
        found = true;
    }
    if (!found) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    *output = result;
    return true;
}
static bool pe_header(PeVersionReader *reader) {
    uint32_t magic, pe, signature, machine, count, optional_size, optional_magic, directories;
    if (!pe_read(reader, 0, 2, &magic)) return false;
    if (magic != 0x5a4d) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    if (!pe_read(reader, 60, 4, &pe)) return false;
    if (pe < 64 || (pe & 3)) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    if (!pe_range(reader, pe, 24) || !pe_read(reader, pe, 4, &signature)) return false;
    if (signature != 0x00004550) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    if (!pe_read(reader, (size_t)pe + 4, 2, &machine) || !pe_read(reader, (size_t)pe + 6, 2, &count) ||
        !pe_read(reader, (size_t)pe + 20, 2, &optional_size)) return false;
    if (machine != 0x8664) return pe_fail(reader, XTC_XIR_PE_VERSION_UNSUPPORTED);
    size_t optional = (size_t)pe + 24;
    if (optional_size < 2) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    if (!pe_range(reader, optional, optional_size) || !pe_read(reader, optional, 2, &optional_magic)) return false;
    if (optional_magic != 0x20b) return pe_fail(reader, XTC_XIR_PE_VERSION_UNSUPPORTED);
    if (optional_size < 112 || !count) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    if (!pe_read(reader, optional + 108, 4, &directories)) return false;
    if (directories > (optional_size - 112) / 8) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    reader->sections = optional + optional_size;
    reader->section_count = (uint16_t)count;
    if (!pe_range(reader, reader->sections, (size_t)count * 40)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = reader->sections + (size_t)i * 40;
        uint32_t virtual_size, address, raw_size, raw;
        if (!pe_read(reader, at + 8, 4, &virtual_size) || !pe_read(reader, at + 12, 4, &address) ||
            !pe_read(reader, at + 16, 4, &raw_size) || !pe_read(reader, at + 20, 4, &raw)) return false;
        uint32_t mapped = virtual_size > raw_size ? virtual_size : raw_size;
        if (mapped > UINT32_MAX - address) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
        if (raw_size && !pe_range(reader, raw, raw_size)) return false;
    }
    if (directories < 3) return pe_fail(reader, XTC_XIR_PE_VERSION_NOT_FOUND);
    uint32_t rva, bytes;
    if (!pe_read(reader, optional + 128, 4, &rva) || !pe_read(reader, optional + 132, 4, &bytes)) return false;
    if (!rva && !bytes) return pe_fail(reader, XTC_XIR_PE_VERSION_NOT_FOUND);
    if (!rva || bytes < 16 || (rva & 3)) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    reader->resource_size = bytes;
    return pe_rva(reader, rva, bytes, &reader->resource);
}
static bool pe_resource_range(PeVersionReader *reader, uint32_t offset, uint32_t length, unsigned alignment) {
    return (!(offset % alignment) && offset <= reader->resource_size && length <= reader->resource_size - offset) ||
        pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
}
static bool pe_name(PeVersionReader *reader, uint32_t name) {
    if (!(name & 0x80000000u)) return name <= UINT16_MAX || pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    uint32_t offset = name & 0x7fffffffu, length;
    if (!pe_resource_range(reader, offset, 2, 2) ||
        !pe_read(reader, reader->resource + offset, 2, &length)) return false;
    return pe_resource_range(reader, offset, 2 + length * 2, 2);
}
static bool pe_fixed(PeVersionReader *reader, uint32_t offset) {
    if (!pe_resource_range(reader, offset, 16, 4)) return false;
    size_t entry = reader->resource + offset;
    uint32_t rva, length, codepage, reserved;
    if (!pe_read(reader, entry, 4, &rva) || !pe_read(reader, entry + 4, 4, &length) ||
        !pe_read(reader, entry + 8, 4, &codepage) || !pe_read(reader, entry + 12, 4, &reserved)) return false;
    (void)codepage;
    if (reserved || (rva & 3)) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    size_t at;
    if (!pe_rva(reader, rva, length, &at)) return false;
    if (length < 6) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    uint32_t total, value_size, type;
    if (!pe_read(reader, at, 2, &total) || !pe_read(reader, at + 2, 2, &value_size) ||
        !pe_read(reader, at + 4, 2, &type)) return false;
    if (total > length || total < 92 || value_size != 52 || type) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    static const char key[] = "VS_VERSION_INFO";
    for (unsigned i = 0; i < sizeof(key); ++i) {
        uint32_t unit;
        if (!pe_read(reader, at + 6 + (size_t)i * 2, 2, &unit) || !pe_work(reader, 1)) return false;
        if (unit != (uint8_t)key[i]) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    }
    uint32_t fixed[13];
    for (unsigned i = 0; i < 13; ++i)
        if (!pe_read(reader, at + 40 + (size_t)i * 4, 4, &fixed[i])) return false;
    if (fixed[0] != 0xfeef04bdu || (fixed[7] & 0x10u)) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    if (fixed[1] != 0x00010000) return pe_fail(reader, XTC_XIR_PE_VERSION_UNSUPPORTED);
    for (unsigned i = 0; i < 13; ++i) {
        if (reader->found) {
            if (!pe_work(reader, 1)) return false;
            if (fixed[i] != reader->fixed[i]) return pe_fail(reader, XTC_XIR_PE_VERSION_AMBIGUOUS);
        } else {
            if (!pe_work(reader, sizeof(uint32_t))) return false;
            reader->fixed[i] = fixed[i];
        }
    }
    reader->found = true;
    return true;
}
/* Only the three resource-directory levels are followed. Explicit ancestor
 * rejection prevents cycles even when crafted bytes resemble valid entries. */
static bool pe_directory(PeVersionReader *reader, uint32_t offset, unsigned depth, uint32_t ancestors[3]) {
    if (!pe_resource_range(reader, offset, 16, 4)) return false;
    for (unsigned i = 0; i < depth; ++i) {
        if (!pe_work(reader, 1)) return false;
        if (offset == ancestors[i]) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
    }
    ancestors[depth] = offset;
    uint32_t named, ids;
    if (!pe_read(reader, reader->resource + offset + 12, 2, &named) ||
        !pe_read(reader, reader->resource + offset + 14, 2, &ids)) return false;
    uint32_t count = named + ids;
    if (!pe_resource_range(reader, offset, 16 + count * 8, 4)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t entry = reader->resource + offset + 16 + (size_t)i * 8;
        uint32_t name, target;
        if (!pe_read(reader, entry, 4, &name) || !pe_read(reader, entry + 4, 4, &target) || !pe_name(reader, name))
            return false;
        if (((name & 0x80000000u) != 0) != (i < named)) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
        if (depth == 2 && (name & 0x80000000u)) return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
        bool directory = (target & 0x80000000u) != 0;
        uint32_t child = target & 0x7fffffffu;
        if (directory != (depth < 2) || !pe_resource_range(reader, child, 16, 4))
            return pe_fail(reader, XTC_XIR_PE_VERSION_INVALID);
        if (!depth && name != 16) continue;
        if (depth < 2) {
            if (!pe_directory(reader, child, depth + 1, ancestors)) return false;
        } else if (!pe_fixed(reader, child)) return false;
    }
    return true;
}
static bool pe_decimal(PeVersionReader *reader, const uint16_t parts[4], char output[24]) {
    size_t used = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!pe_work(reader, 1)) return false;
        uint32_t value = parts[i];
        char digits[5];
        unsigned count = 0;
        do {
            if (!pe_work(reader, 1)) return false;
            digits[count++] = (char)('0' + value % 10); value /= 10;
        } while (value);
        while (count) {
            if (!pe_work(reader, 1)) return false;
            output[used++] = digits[--count];
        }
        if (i < 3) {
            if (!pe_work(reader, 1)) return false;
            output[used++] = '.';
        }
    }
    if (!pe_work(reader, 1)) return false;
    output[used] = 0;
    return true;
}
XR_FUNC XtcXirPeVersionStatus xtc_xir_pe_version_parse(XrCompileResources *resources,
    const uint8_t *bytes, size_t length, XtcXirPeVersion *output) {
    if (!resources || !bytes || !output) return XTC_XIR_PE_VERSION_BAD_ARGUMENT;
    PeVersionReader reader = {0};
    reader.resources = resources; reader.bytes = bytes; reader.length = length;
    uint32_t ancestors[3];
    if (!pe_header(&reader) || !pe_directory(&reader, 0, 0, ancestors)) return reader.status;
    if (!reader.found) return XTC_XIR_PE_VERSION_NOT_FOUND;
    if (!pe_work(&reader, sizeof(XtcXirPeVersion))) return reader.status;
    XtcXirPeVersion result = {0};
    for (unsigned i = 0; i < 4; ++i) {
        if (!pe_work(&reader, 2)) return reader.status;
        unsigned shift = (i & 1) ? 0 : 16;
        result.file[i] = (uint16_t)(reader.fixed[2 + i / 2] >> shift);
        result.product[i] = (uint16_t)(reader.fixed[4 + i / 2] >> shift);
    }
    if (!pe_decimal(&reader, result.file, result.file_text) || !pe_decimal(&reader, result.product, result.product_text) ||
        !pe_work(&reader, sizeof(result))) return reader.status;
    memcpy(output, &result, sizeof(result));
    return XTC_XIR_PE_VERSION_OK;
}
