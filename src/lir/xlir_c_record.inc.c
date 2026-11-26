/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_record.inc.c - Natural Record values over stable inline owner homes
 *
 * KEY CONCEPT:
 *   Typed local snapshots borrow their operand owners only through the Core
 *   call. All owned results publish through their real Frame leaf slots.
 */
static bool c_record_point(const XrLirInstructionView *v) {
    return v->op == XR_LIR_RECORD_NEW || v->op == XR_LIR_RECORD_FIELD ||
        v->op == XR_LIR_CLASS_NEW || v->op == XR_LIR_CLASS_FIELD ||
        v->op == XR_LIR_CLASS_ADOPT ||
        v->op == XR_LIR_HOST_FILE_OPEN_WRITE || v->op == XR_LIR_HOST_FILE_CLOSE || v->op == XR_LIR_HOST_FILE_WRITE ||
        v->op == XR_LIR_HOST_PATH_CLASS || v->op == XR_LIR_HOST_FILE_READ || v->op == XR_LIR_HOST_ENV_GET ||
        v->op == XR_LIR_HOST_TEXT_RESULT || v->op == XR_LIR_HOST_DIRECTORY ||
        v->op == XR_LIR_HOST_DIRECTORY_CLEANUP_CLOSE || v->op == XR_LIR_HOST_DIRECTORY_CLEANUP_POLL ||
        v->op == XR_LIR_CLASS_SCALAR_WRITE || v->op == XR_LIR_CLASS_STRING_WRITE ||
        v->op == XR_LIR_CLASS_ARRAY_WRITE || v->op == XR_LIR_CLASS_ARRAY_PUSH ||
        v->op == XR_LIR_VALUE_RETAIN || v->op == XR_LIR_VALUE_DROP;
}
static void c_host_file_read_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    /* Both numbered Path calls use the existing unary nullable class ABI. */
    bool path_class = v->op == XR_LIR_HOST_PATH_CLASS;
    const XrLirValueHome *home = &c->homes->values[v->result];
    const XrtValueImage *image = xr_lir_value_image_view(c->value_image);
    const XrtValueImageType *optional = &image->types[home->type - 1];
    XR_CHECK(optional->kind == XRT_VALUE_NULLABLE && home->temporary_offset != SIZE_MAX &&
        optional->nullable_payload <= SIZE_MAX - home->temporary_offset,
        "verified file result has a stable nullable owner offset");
    uint32_t class_type = path_class ? optional->nullable_element : c->homes->values[v->operands[0]].type;
    c_values_ensure(c, source);
    c_format(c, "    xrt_native_host_file_%s(UINT32_C(%" PRIu64 "), &(XrtNativeFile%s){"
        ".output={xrl_frame, %zu, %zu, %zu}, .temporary_owner_offset=%zu, "
        ".image=&xrl_value_image, .type=UINT32_C(%" PRIu32 "), .result_type=UINT32_C(%" PRIu32 "), "
        ".%s=(const XrtManagedHeader *)", path_class ? "open_read" : "read", v->bits, path_class ? "OpenRead" : "Read",
        home->first_owner, home->temporary_first_owner, home->offset,
        home->temporary_offset + optional->nullable_payload, class_type, home->type, path_class ? "path" : "file");
    c_value(c, v->operands[0]);
    if (!path_class) {
        c_text(c, ", .max_bytes=");
        c_value(c, v->operands[1]);
    }
    c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
}
static void c_host_file_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_HOST_PATH_CLASS || v->op == XR_LIR_HOST_FILE_READ) {
        c_host_file_read_instruction(c, v, source);
        return;
    }
    if (v->op == XR_LIR_HOST_FILE_WRITE) {
        uint32_t type = c->homes->values[v->operands[0]].type;
        c_format(c, "    v%" PRIu32 " = xrt_native_host_file_write(UINT32_C(%" PRIu64
            "), &(XrtNativeFileWrite){.image=&xrl_value_image, .type=UINT32_C(%" PRIu32 "), .file=",
            v->result, v->bits, type);
        c_value(c, v->operands[0]);
        c_text(c, ", .data=(const XrtManagedHeader *)");
        c_value(c, v->operands[1]);
        c_text(c, ", .offset=");
        c_value(c, v->operands[2]);
        c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    if (v->op == XR_LIR_HOST_FILE_CLOSE) {
        uint32_t type = c->homes->values[v->operands[0]].type;
        c_format(c, "    v%" PRIu32 " = xrt_native_host_file_close(UINT32_C(%" PRIu64
            "), &xrl_value_image, UINT32_C(%" PRIu32 "), ", v->result, v->bits, type);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    c_values_ensure(c, source);
    const XrLirValueHome *home = &c->homes->values[v->result];
    const XrtValueImage *image = xr_lir_value_image_view(c->value_image);
    const XrtValueImageType *nullable = &image->types[home->type - 1];
    c_format(c, "    xrt_native_host_file_open_write(UINT32_C(%" PRIu64 "), &(XrtNativeFileOpen){"
        ".output={xrl_frame, %zu, %zu, %zu}, "
        ".image=&xrl_value_image, .type=UINT32_C(%" PRIu32 "), .path=(const XrtStringBacking *)",
        v->bits, home->first_owner, home->temporary_first_owner, home->offset,
        nullable->nullable_element);
    c_value(c, v->operands[0]);
    c_text(c, ", .append="); c_value(c, v->operands[1]);
    c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
}
static void c_class_array_field(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    bool read = v->op == XR_LIR_CLASS_FIELD;
    bool push = v->op == XR_LIR_CLASS_ARRAY_PUSH;
    const XrLirValueHome *object = &c->homes->values[v->operands[0]];
    const XrLirHomeWorkspace *workspace = push ? c_array_workspace(c) : NULL;
    if (push && !workspace) return;
    XR_CHECK(!push || (workspace->kind == XR_LIR_HOME_WORKSPACE_CLASS_ARRAY_PUSH && workspace->value == v->operands[0]),
        "verified Class Array push has its own concrete replacement slot");
    if (read || push) c_values_ensure(c,source);
    c_format(c,"    xrt_native_class_array_field_%s(&(XrtNativeClassField){",read ? "copy" : push ? "push" : "replace");
    c_value(c,v->operands[0]);
    c_format(c,", &xrl_value_image, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu64 ")}", object->type, v->bits);
    if (read) c_format(c,", xrl_frame, %zu", c->homes->values[v->result].temporary_first_owner);
    if (push) c_format(c,", xrl_frame, %zu", workspace->first_owner);
    if (!read) { c_text(c,", "); c_value(c,v->operands[1]); }
    c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
    if (read) c_values_publish_leaf(c,v->result,source);
}
static void c_class_string_field(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    bool read = v->op == XR_LIR_CLASS_FIELD;
    XrLirType type = read ? v->type : c_value_type(c, v->operands[1]);
    bool nullable = type != XR_LIR_STRING;
    const XrLirValueHome *object = &c->homes->values[v->operands[0]];
    if (read) c_values_ensure(c, source);
    c_format(c, "    xrt_native_class_string_field_%s(&(XrtNativeClassField){", read ? "copy" : "replace");
    c_value(c, v->operands[0]);
    c_format(c, ", &xrl_value_image, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu64 ")}", object->type, v->bits);
    if (read) {
        const XrLirValueHome *home = &c->homes->values[v->result];
        XR_CHECK(home->owner_count == 1 && home->offset != SIZE_MAX,
            "verified String field snapshot has one stable inline owner");
        c_format(c, ", xrl_frame, %zu, ", home->first_owner);
        if (nullable) c_format(c, "%zu", home->offset);
        else c_text(c, "SIZE_MAX");
    } else {
        c_text(c, ", (const XrtManagedHeader *)");
        c_value(c, v->operands[1]);
        if (nullable) {
            c_text(c, ".payload, ");
            c_value(c, v->operands[1]);
            c_text(c, ".tag, true");
        } else c_text(c, ", UINT8_C(1), false");
    }
    c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
}
static void c_host_environment_get(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    const XrLirValueHome *home = &c->homes->values[v->result];
    const XrtValueImage *image = xr_lir_value_image_view(c->value_image);
    const XrtValueImageType *optional = &image->types[home->type - 1];
    XR_CHECK(optional->kind == XRT_VALUE_NULLABLE && home->temporary_offset != SIZE_MAX &&
        optional->nullable_payload <= SIZE_MAX - home->temporary_offset,
        "verified environment result has a stable nullable owner offset");
    c_values_ensure(c, source);
    c_format(c, "    xrt_native_host_environment_get(UINT32_C(%" PRIu64 "), &(XrtNativeEnvironmentGet){"
        ".frame=xrl_frame, .destination=%zu, .temporary=%zu, .offset=%zu, .temporary_owner_offset=%zu, "
        ".image=&xrl_value_image, .result_type=UINT32_C(%" PRIu32 "), .name=(const XrtManagedHeader *)",
        v->bits, home->first_owner, home->temporary_first_owner, home->offset,
        home->temporary_offset + optional->nullable_payload, home->type);
    c_value(c, v->operands[0]);
    c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
}
static void c_host_text_result(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    const XrLirValueHome *home = &c->homes->values[v->result];
    const XrtValueImage *image = xr_lir_value_image_view(c->value_image);
    const XrtValueImageType *optional = &image->types[home->type - 1];
    XR_CHECK(optional->kind == XRT_VALUE_NULLABLE && home->temporary_offset != SIZE_MAX &&
        optional->nullable_payload <= SIZE_MAX - home->temporary_offset,
        "verified text result has a stable nullable owner offset");
    c->host_text_result = true;
    c_values_ensure(c, source);
    c_format(c, "    xrt_native_host_text_result(UINT32_C(%" PRIu64 "), &(XrtNativeHostTextResult){"
        ".frame=xrl_frame, .destination=%zu, .temporary=%zu, .offset=%zu, .temporary_owner_offset=%zu, "
        ".image=&xrl_value_image, .result_type=UINT32_C(%" PRIu32 "), .operand=(const XrtManagedHeader *)",
        v->bits, home->first_owner, home->temporary_first_owner, home->offset,
        home->temporary_offset + optional->nullable_payload, home->type);
    c_value(c, v->operands[0]);
    c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
}
static void c_host_directory(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->bits == XR_NATIVE_CALLABLE_IO_DIR_OPEN) {
        c_format(c, "    v%" PRIu32 " = xrt_native_host_directory_open((const XrtManagedHeader *)", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    if (v->bits == XR_NATIVE_CALLABLE_IO_DIR_CLOSE) {
        c_format(c, "    v%" PRIu32 " = xrt_native_host_directory_close(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    XR_CHECK(v->bits == XR_NATIVE_CALLABLE_IO_DIR_NEXT, "verified directory operation has a known callable");
    const XrLirValueHome *home = &c->homes->values[v->result];
    const XrtValueImage *image = xr_lir_value_image_view(c->value_image);
    const XrtValueImageType *optional = &image->types[home->type - 1];
    XR_CHECK(optional->kind == XRT_VALUE_NULLABLE && home->temporary_offset != SIZE_MAX &&
        optional->nullable_payload <= SIZE_MAX - home->temporary_offset,
        "verified directory next has a stable nullable owner offset");
    c_values_ensure(c, source);
    c_format(c, "    xrt_native_host_directory_next(&(XrtNativeDirectoryNext){"
        ".frame=xrl_frame, .destination=%zu, .temporary=%zu, .offset=%zu, .temporary_owner_offset=%zu, "
        ".image=&xrl_value_image, .result_type=UINT32_C(%" PRIu32 "), .token=",
        home->first_owner, home->temporary_first_owner, home->offset,
        home->temporary_offset + optional->nullable_payload, home->type);
    c_value(c, v->operands[0]);
    c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
}

static void c_record_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_HOST_DIRECTORY) {
        c_host_directory(c, v, source);
        return;
    }
    if (v->op == XR_LIR_HOST_DIRECTORY_CLEANUP_CLOSE) {
        c_text(c, "    xrt_native_host_directory_cleanup_close(");
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    if (v->op == XR_LIR_HOST_DIRECTORY_CLEANUP_POLL) {
        c_format(c, "    xrt_native_host_directory_cleanup_poll(UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    if (v->op == XR_LIR_HOST_ENV_GET) {
        c_host_environment_get(c, v, source);
        return;
    }
    if (v->op == XR_LIR_HOST_TEXT_RESULT) {
        c_host_text_result(c, v, source);
        return;
    }
    if (v->op == XR_LIR_HOST_FILE_OPEN_WRITE || v->op == XR_LIR_HOST_FILE_CLOSE || v->op == XR_LIR_HOST_FILE_WRITE ||
        v->op == XR_LIR_HOST_PATH_CLASS || v->op == XR_LIR_HOST_FILE_READ) {
        c_host_file_instruction(c, v, source);
        return;
    }
    if (v->op == XR_LIR_CLASS_ARRAY_WRITE || v->op == XR_LIR_CLASS_ARRAY_PUSH) {
        c_class_array_field(c,v,source);
        return;
    }
    if (v->op == XR_LIR_CLASS_STRING_WRITE) {
        c_class_string_field(c, v, source);
        return;
    }
    if (v->op == XR_LIR_CLASS_SCALAR_WRITE) {
        const XrLirValueHome *object = &c->homes->values[v->operands[0]];
        c_format(c, "    { const %s input = ", c_type(c,c_value_type(c,v->operands[1])));
        c_value(c,v->operands[1]);
        c_text(c, ";\n        xrt_native_class_scalar_field_write(&(XrtNativeClassField){");
        c_value(c,v->operands[0]);
        c_format(c, ", &xrl_value_image, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu64 ")}, &input, sizeof(input), UINT32_C(%" PRIu32 "));\n    }\n",
            object->type,v->bits,source);
        return;
    }
    uint32_t destination = v->op == XR_LIR_VALUE_DROP ? v->operands[0] : v->result;
    bool owned = c->homes && c->homes->values[destination].owner_count;
    if (v->op == XR_LIR_VALUE_DROP) {
        if (owned) { c_values_replace_begin(c, destination, source); c_text(c, "NULL"); c_values_replace_end(c, source); }
        return;
    }
    bool class_new = v->op == XR_LIR_CLASS_NEW;
    if (class_new || v->op == XR_LIR_RECORD_NEW) {
        XrLirType row = v->type;
        if (class_new) XR_CHECK(xr_lir_class_payload(c->request->module, v->type, &row),
            "verified class creation has a complete nominal payload");
        c_format(c, "    { const %s input = {", c_type(c, row));
        if (!v->operand_count) c_text(c, "0");
        for (uint32_t a = 0; a < v->operand_count && c_request_poll(c); ++a) {
            if (a) c_text(c, ", ");
            c_value(c, v->operands[a]);
        }
        c_text(c, "};\n");
        if (class_new) {
            c_values_ensure(c, source);
            const XrLirValueHome *home = &c->homes->values[destination];
            c_format(c, "    xrt_native_class_create(xrl_frame, %zu, &xrl_value_image, UINT32_C(%" PRIu32
                "), &input, UINT32_C(%" PRIu32 "));\n", home->temporary_first_owner, home->type, source);
            c_values_publish_leaf(c, destination, source);
        } else if (owned) {
            c_values_replace_begin(c, destination, source); c_text(c, "&input"); c_values_replace_end(c, source);
        } else c_format(c, "    v%" PRIu32 " = input;\n", destination);
        c_text(c, "    }\n"); return;
    }
    if (v->op == XR_LIR_CLASS_FIELD) {
        const XrLirValueHome *object = &c->homes->values[v->operands[0]];
        const XrtValueImage *image = xr_lir_value_image_view(c->value_image);
        const XrtValueImageType *class_type = &image->types[object->type-1];
        const XrtValueImageType *row = &image->types[class_type->element-1];
        if (image->slots[row->first_field + v->bits].flags & XRT_VALUE_FIELD_MUTABLE) {
            if (v->type == XR_LIR_ARRAY) {
                c_class_array_field(c, v, source);
                return;
            }
            if (xr_lir_class_string_field_supported(c->request->module, v->type)) {
                c_class_string_field(c, v, source);
                return;
            }
        }
        if (owned) c_values_ensure(c, source);
        c_text(c, "    { const void *field = xrt_native_class_field("); c_value(c, v->operands[0]);
        c_format(c, ", &xrl_value_image, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu64 "), UINT32_C(%" PRIu32 "));\n",
            object->type, v->bits, source);
        if (owned) {
            c_values_replace_begin(c, destination, source); c_text(c, "field"); c_values_replace_end(c, source);
        } else c_format(c, "    memcpy(&v%" PRIu32 ", field, sizeof(v%" PRIu32 "));\n", destination, destination);
        c_text(c, "    }\n");
        return;
    }
    if (owned) { c_values_replace_begin(c, destination, source); c_text(c, "&"); }
    else c_format(c, "    v%" PRIu32 " = ", destination);
    c_value(c, v->operands[0]);
    if (v->op == XR_LIR_RECORD_FIELD) c_format(c, ".f%" PRIu64, v->bits);
    if (owned) c_values_replace_end(c, source); else c_text(c, ";\n");
}
