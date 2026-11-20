/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_value_image.inc.c - Static C data from the common raw value image
 *
 * KEY CONCEPT:
 *   Emit the serializer's exact vectors. Type ids, spans, layouts and owned
 *   leaves are copied facts; no C-specific physical catalog is reconstructed.
 */
static void c_value_image(LirC *c, const XrtValueImage *image) {
    if (image->type_count) c_text(c, "static const XrtValueImageType xrl_value_types[] = {\n");
    for (uint32_t i = 0; i < image->type_count && c_request_poll(c); ++i) {
        const XrtValueImageType *row = &image->types[i];
        c_format(c, "    {.kind=(XrtValueKind)%u,\n", (unsigned)row->kind);
#define C_IMAGE_SIZE(field) c_format(c, "     ." #field "=UINT64_C(%" PRIu64 "),\n", (uint64_t)row->field)
#define C_IMAGE_U32(field) c_format(c, "     ." #field "=UINT32_C(%" PRIu32 "),\n", row->field)
        C_IMAGE_SIZE(size); C_IMAGE_SIZE(alignment); C_IMAGE_SIZE(count);
        C_IMAGE_U32(first_owned); C_IMAGE_U32(owned_count);
        C_IMAGE_U32(first_field); C_IMAGE_U32(field_count); C_IMAGE_U32(first_capture); C_IMAGE_U32(capture_count);
        C_IMAGE_U32(first_parameter); C_IMAGE_U32(parameter_count);
        C_IMAGE_U32(nullable_element); C_IMAGE_U32(result); C_IMAGE_U32(flags); C_IMAGE_U32(throws);
        C_IMAGE_SIZE(nullable_tag); C_IMAGE_SIZE(nullable_payload);
        C_IMAGE_U32(entry_ordinal); C_IMAGE_U32(entry_signature); C_IMAGE_U32(element);
        if (row->class_parent) C_IMAGE_U32(class_parent);
#undef C_IMAGE_U32
#undef C_IMAGE_SIZE
        c_text(c, "    },\n");
    }
    if (image->type_count) c_text(c, "};\n");
    if (image->slot_count) c_text(c, "static const XrtValueImageSlot xrl_value_slots[] = {\n");
    for (uint32_t i = 0; i < image->slot_count && c_request_poll(c); ++i) {
        XrtValueImageSlot row = image->slots[i];
        c_format(c, "    {UINT32_C(%" PRIu32 "), UINT64_C(%" PRIu64 "), UINT64_C(%" PRIu64 "), UINT64_C(%" PRIu64 ")},\n",
            row.type, (uint64_t)row.offset, (uint64_t)row.name_offset, (uint64_t)row.name_length);
    }
    if (image->slot_count) c_text(c, "};\n");
    if (image->parameter_count) c_text(c, "static const XrtValueImageParameter xrl_value_parameters[] = {\n");
    for (uint32_t i = 0; i < image->parameter_count && c_request_poll(c); ++i) {
        XrtValueImageParameter row = image->parameters[i];
        c_format(c, "    {UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), UINT64_C(%" PRIu64 "), UINT64_C(%" PRIu64 ")},\n",
            row.type, row.mode, row.escaping, (uint64_t)row.name_offset, (uint64_t)row.name_length);
    }
    if (image->parameter_count) c_text(c, "};\n");
    if (image->owned_count) c_text(c, "static const size_t xrl_value_owned[] = {\n");
    for (uint32_t i = 0; i < image->owned_count && c_request_poll(c); ++i)
        c_format(c, "    UINT64_C(%" PRIu64 "),\n", (uint64_t)image->owned_offsets[i]);
    if (image->owned_count) c_text(c, "};\n");
    if (image->name_bytes) c_text(c, "static const unsigned char xrl_value_names[] = {\n");
    for (size_t i = 0; i < image->name_bytes && c_request_poll(c); ++i)
        c_format(c, "0x%02x,", (unsigned)(uint8_t)image->names[i]);
    if (image->name_bytes) c_text(c, "\n};\n");
    c_text(c, "static const XrtValueImage xrl_value_image = {\n");
    c_format(c, "    %s, UINT32_C(%" PRIu32 "), %s, UINT32_C(%" PRIu32 "),\n",
        image->type_count ? "xrl_value_types" : "NULL", image->type_count,
        image->slot_count ? "xrl_value_slots" : "NULL", image->slot_count);
    c_format(c, "    %s, UINT32_C(%" PRIu32 "), %s, UINT32_C(%" PRIu32 "),\n",
        image->parameter_count ? "xrl_value_parameters" : "NULL", image->parameter_count,
        image->owned_count ? "xrl_value_owned" : "NULL", image->owned_count);
    c_format(c, "    %s, UINT64_C(%" PRIu64 ")\n};\n",
        image->name_bytes ? "(const char *)xrl_value_names" : "NULL", (uint64_t)image->name_bytes);
}
