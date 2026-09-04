/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 */

#include "xanalyzer_capability.h"

#include "xanalyzer_builtins.h"
#include "xanalyzer_symbol.h"
#include "../../runtime/class/xclass_info.h"
#include "../../shared/xr_derive_flags.h"

#include <stdio.h>
#include <string.h>

static bool path_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix)
        return false;
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len || strcmp(path + path_len - suffix_len, suffix) != 0)
        return false;
    return path_len == suffix_len || path[path_len - suffix_len - 1] == '/' ||
           path[path_len - suffix_len - 1] == '\\';
}

typedef struct XaDeclaredTypeCapabilitySpec {
    const char *module_name;
    const char *declaration_name;
    uint32_t flags;
} XaDeclaredTypeCapabilitySpec;

static const XaDeclaredTypeCapabilitySpec declared_type_capabilities[] = {
#define XA_TYPE_CAPABILITY(module_name, declaration_name, flags)                                   \
    {module_name, declaration_name, flags},
#include "xa_type_capability_registry.def"
#undef XA_TYPE_CAPABILITY
};

static bool declaration_identity_matches(const char *actual, const char *declared) {
    if (!actual || !declared)
        return false;
    size_t declared_len = strlen(declared);
    return strncmp(actual, declared, declared_len) == 0 &&
           (actual[declared_len] == '\0' || actual[declared_len] == '$');
}

static bool path_is_canonical_stdlib_module(const char *path, const char *module_name) {
    if (!path || !module_name)
        return false;
    char suffix[160];
    int n = snprintf(suffix, sizeof(suffix), "stdlib/%s/%s.xr", module_name, module_name);
    if (n > 0 && (size_t) n < sizeof(suffix) && path_has_suffix(path, suffix))
        return true;
    n = snprintf(suffix, sizeof(suffix), "<embedded stdlib>/%s/%s.xr", module_name, module_name);
    return n > 0 && (size_t) n < sizeof(suffix) && path_has_suffix(path, suffix);
}

uint32_t xa_stdlib_type_capability_flags(const char *module_name, const char *declaration_name) {
    if (!module_name || !declaration_name)
        return XA_TYPE_CAP_NONE;
    for (size_t i = 0;
         i < sizeof(declared_type_capabilities) / sizeof(declared_type_capabilities[0]); i++) {
        const XaDeclaredTypeCapabilitySpec *spec = &declared_type_capabilities[i];
        if (strcmp(module_name, spec->module_name) == 0 &&
            declaration_identity_matches(declaration_name, spec->declaration_name))
            return spec->flags;
    }
    return XA_TYPE_CAP_NONE;
}

uint32_t xa_declared_type_capability_flags(const char *file_path, const char *declaration_name) {
    if (!file_path || !declaration_name)
        return XA_TYPE_CAP_NONE;
    for (size_t i = 0;
         i < sizeof(declared_type_capabilities) / sizeof(declared_type_capabilities[0]); i++) {
        const XaDeclaredTypeCapabilitySpec *spec = &declared_type_capabilities[i];
        if (declaration_identity_matches(declaration_name, spec->declaration_name) &&
            path_is_canonical_stdlib_module(file_path, spec->module_name))
            return spec->flags;
    }
    return XA_TYPE_CAP_NONE;
}

static uint32_t native_type_capability_flags(const XrType *type) {
    if (!type)
        return XA_TYPE_CAP_NONE;
    switch (xr_type_to_builtin_id((XrType *) type)) {
        case XR_TID_CHANNEL:
        case XR_TID_ATOMIC:
            return XA_TYPE_CAP_INTERIOR_MUTABLE | XA_TYPE_CAP_SYNC_SHAREABLE;
        default:
            return XA_TYPE_CAP_NONE;
    }
}

uint32_t xa_type_capability_flags(const XrType *type) {
    uint32_t flags = native_type_capability_flags(type);
    if (type && XR_TYPE_IS_INSTANCE(type) && type->instance.class_ref)
        flags |= type->instance.class_ref->capability_flags;
    return flags;
}

bool xa_type_has_capabilities(const XrType *type, uint32_t required) {
    return (xa_type_capability_flags(type) & required) == required;
}

typedef enum XaJsonCapabilityMode {
    XA_JSON_CAPABILITY_ENCODE,
    XA_JSON_CAPABILITY_DECODE,
} XaJsonCapabilityMode;

typedef struct XaJsonCapabilityWalk {
    const XrType *stack[64];
    int depth;
} XaJsonCapabilityWalk;

static XaJsonCapabilityResult json_capability_result(bool supported, XaJsonCapabilityReason reason,
                                                     const XrType *blocking_type) {
    XaJsonCapabilityResult result = {
        .supported = supported,
        .reason = reason,
        .blocking_type = blocking_type,
    };
    return result;
}

static bool json_capability_stack_contains(const XaJsonCapabilityWalk *walk, const XrType *type) {
    for (int i = 0; walk && i < walk->depth; i++) {
        if (walk->stack[i] == type)
            return true;
    }
    return false;
}

static XaJsonCapabilityResult json_capability_visit(const XrType *type, XaJsonCapabilityMode mode,
                                                    XaJsonCapabilityWalk *walk) {
    if (!type)
        return json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
    if (xr_type_is_json_value(type))
        return json_capability_result(true, XA_JSON_CAPABILITY_OK, NULL);

    switch (type->kind) {
        case XR_KIND_RUNE:
            return mode == XA_JSON_CAPABILITY_ENCODE
                       ? json_capability_result(true, XA_JSON_CAPABILITY_OK, NULL)
                       : json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
        case XR_KIND_FUNCTION:
            return json_capability_result(false, XA_JSON_CAPABILITY_FUNCTION_FIELD, type);
        case XR_KIND_TYPE_PARAM: {
            const char *required =
                mode == XA_JSON_CAPABILITY_ENCODE ? "JSON.Encodable" : "JSON.Decodable";
            if (type->type_param.constraint &&
                xr_type_is_builtin_named_type(type->type_param.constraint, required))
                return json_capability_result(true, XA_JSON_CAPABILITY_OK, NULL);
            return json_capability_result(false, XA_JSON_CAPABILITY_UNINSTANTIATED_TYPE_PARAMETER,
                                          type);
        }
        case XR_KIND_ENUM:
            if (mode == XA_JSON_CAPABILITY_ENCODE ||
                (type->enum_type.layout && type->enum_type.layout->is_zero_payload &&
                 type->enum_type.layout->variant_count > 0))
                return json_capability_result(true, XA_JSON_CAPABILITY_OK, NULL);
            return json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
        case XR_KIND_INSTANCE:
            if (!type->instance.class_ref) {
                return json_capability_result(false,
                                              mode == XA_JSON_CAPABILITY_ENCODE
                                                  ? XA_JSON_CAPABILITY_NON_ENCODABLE_NATIVE_HANDLE
                                                  : XA_JSON_CAPABILITY_NON_DECODABLE_NATIVE_HANDLE,
                                              type);
            }
            if ((type->instance.class_ref->derive_flags & XR_DERIVE_JSON) == 0) {
                return json_capability_result(false, XA_JSON_CAPABILITY_MISSING_DERIVE_SIDECAR,
                                              type);
            }
            break;
        case XR_KIND_TUPLE:
            if (mode == XA_JSON_CAPABILITY_DECODE) {
                return json_capability_result(false, XA_JSON_CAPABILITY_TUPLE_TARGET_NOT_DECODABLE,
                                              type);
            }
            break;
        case XR_KIND_STRUCT_OBJECT:
            break;
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
            break;
        case XR_KIND_SET:
            if (mode == XA_JSON_CAPABILITY_ENCODE)
                break;
            return json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
        case XR_KIND_UNION:
            if (mode == XA_JSON_CAPABILITY_ENCODE)
                break;
            return json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
        default:
            return json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
    }

    if (json_capability_stack_contains(walk, type) || walk->depth >= 64) {
        return json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_RECURSIVE_ALIAS, type);
    }
    walk->stack[walk->depth++] = type;

    XaJsonCapabilityResult result = json_capability_result(true, XA_JSON_CAPABILITY_OK, NULL);
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
            result = json_capability_visit(type->container.element_type, mode, walk);
            break;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                result = json_capability_visit(type->union_type.members[i], mode, walk);
                if (!result.supported)
                    break;
            }
            break;
        case XR_KIND_MAP:
            if (!type->map.key_type || !XR_TYPE_IS_STRING(type->map.key_type) ||
                type->map.key_type->is_nullable) {
                result = json_capability_result(false, XA_JSON_CAPABILITY_NON_STRING_MAP_KEY, type);
            } else {
                result = json_capability_visit(type->map.value_type, mode, walk);
            }
            break;
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++) {
                const XrType *element =
                    type->tuple.element_types ? type->tuple.element_types[i] : NULL;
                result = json_capability_visit(element, mode, walk);
                if (!result.supported)
                    break;
            }
            break;
        case XR_KIND_STRUCT_OBJECT:
            for (int i = 0; i < type->object.field_count; i++) {
                const XrType *field = type->object.field_types ? type->object.field_types[i] : NULL;
                result = json_capability_visit(field, mode, walk);
                if (!result.supported)
                    break;
            }
            break;
        case XR_KIND_INSTANCE:
            for (const XrClassInfo *info = type->instance.class_ref; info; info = info->base) {
                for (int i = 0; i < info->field_count; i++) {
                    const XaSymbol *field = info->fields ? info->fields[i] : NULL;
                    if (!field || field->is_static)
                        continue;
                    result = json_capability_visit(field->links.type, mode, walk);
                    if (!result.supported)
                        break;
                }
                if (!result.supported)
                    break;
            }
            break;
        default:
            result = json_capability_result(false, XA_JSON_CAPABILITY_UNSUPPORTED_TYPE, type);
            break;
    }
    walk->depth--;
    return result;
}

XaJsonCapabilityResult xa_json_encodable(const XrType *type) {
    XaJsonCapabilityWalk walk = {0};
    return json_capability_visit(type, XA_JSON_CAPABILITY_ENCODE, &walk);
}

XaJsonCapabilityResult xa_json_decodable(const XrType *type) {
    XaJsonCapabilityWalk walk = {0};
    return json_capability_visit(type, XA_JSON_CAPABILITY_DECODE, &walk);
}

const char *xa_json_capability_reason_name(XaJsonCapabilityReason reason) {
    switch (reason) {
        case XA_JSON_CAPABILITY_OK:
            return "OK";
        case XA_JSON_CAPABILITY_FUNCTION_FIELD:
            return "FUNCTION_FIELD";
        case XA_JSON_CAPABILITY_NON_STRING_MAP_KEY:
            return "NON_STRING_MAP_KEY";
        case XA_JSON_CAPABILITY_UNSUPPORTED_RECURSIVE_ALIAS:
            return "UNSUPPORTED_RECURSIVE_ALIAS";
        case XA_JSON_CAPABILITY_MISSING_DERIVE_SIDECAR:
            return "MISSING_DERIVE_SIDECAR";
        case XA_JSON_CAPABILITY_NON_ENCODABLE_NATIVE_HANDLE:
            return "NON_ENCODABLE_NATIVE_HANDLE";
        case XA_JSON_CAPABILITY_NON_DECODABLE_NATIVE_HANDLE:
            return "NON_DECODABLE_NATIVE_HANDLE";
        case XA_JSON_CAPABILITY_UNINSTANTIATED_TYPE_PARAMETER:
            return "UNINSTANTIATED_TYPE_PARAMETER";
        case XA_JSON_CAPABILITY_TUPLE_TARGET_NOT_DECODABLE:
            return "TUPLE_TARGET_NOT_DECODABLE";
        case XA_JSON_CAPABILITY_UNSUPPORTED_TYPE:
        default:
            return "UNSUPPORTED_TYPE";
    }
}
