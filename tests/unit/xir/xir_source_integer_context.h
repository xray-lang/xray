/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_integer_context.h - Literal bounds and lossless source conversions
 *
 * KEY CONCEPT:
 *   Check exact types and conversions before either executor consumes the module.
 */
#ifndef XIR_SOURCE_INTEGER_CONTEXT_H
#define XIR_SOURCE_INTEGER_CONTEXT_H
typedef struct SourceIntegerCase {
    const char *name, *low, *high;
    XrXirType type;
    int64_t minimum, maximum;
} SourceIntegerCase;
static const SourceIntegerCase source_integer_types[] = {
    {"i8", "-128", "127", XR_XIR_I8, -128, 127},
    {"i16", "-32768", "32767", XR_XIR_I16, -32768, 32767},
    {"i32", "-2147483648", "2147483647", XR_XIR_I32, INT32_MIN, INT32_MAX},
    {"i64", "-9223372036854775808", "9223372036854775807", XR_XIR_I64, INT64_MIN, INT64_MAX},
    {"u8", "0", "255", XR_XIR_U8, 0, 255},
    {"u16", "0", "65535", XR_XIR_U16, 0, 65535},
    {"u32", "0", "4294967295", XR_XIR_U32, 0, UINT32_MAX},
    {"u64", "0", "18446744073709551615", XR_XIR_U64, 0, -1}
};
static void source_integer_bounds(const XrXirSourceRequest *request, const char *root) {
    for (unsigned i = 0; i < 8; ++i) {
        const SourceIntegerCase *test = &source_integer_types[i];
        char text[1024];
        int length = snprintf(text, sizeof(text), "const low:%s=(%s)\nconst high:%s=(%s)\n",
            test->name, test->low, test->name, test->high);
        CHECK(length > 0 && (size_t) length < sizeof(text)); write_source(root, text);
        XrXirArtifact *artifact = NULL;
        CHECK(xr_xir_source_check(request, &artifact, NULL) == XR_XIR_OK && artifact);
        const XrXirModule *module = xr_xir_artifact_module(artifact);
        CHECK(module->declarations->slot_count == 2);
        for (uint32_t s = 0; s < 2; ++s) CHECK(module->declarations->slots[s].type == test->type);
        const XrXirFunction *initializer = &module->functions[0];
        CHECK(initializer->instructions[0].op == XR_XIR_CONST_INT && initializer->instructions[0].type == test->type);
        CHECK(initializer->instructions[0].immediate == test->minimum);
        CHECK(initializer->instructions[2].op == XR_XIR_CONST_INT && initializer->instructions[2].type == test->type);
        CHECK(initializer->instructions[2].immediate == test->maximum);
        xr_xir_artifact_free(artifact);
    }
}
static void source_integer_contexts(const XrXirSourceRequest *request, const char *root) {
    source_integer_bounds(request, root);
    unsigned cases = 0;
    for (unsigned first = 0; first < 8; ++first) for (unsigned last = first + 1; last < 8; ++last) {
        if (first / 4 != last / 4) continue;
        const SourceIntegerCase *from = &source_integer_types[first], *to = &source_integer_types[last];
        char text[2048];
        int length = snprintf(text, sizeof(text),
            "const small:%s=1\nconst bound:%s=small\n"
            "fn widen(x:%s)->%s{return x}\nfn take(x:%s)->%s{return x}\n"
            "const arg=take(small)\nvar assigned:%s=0\nassigned=small\n"
            "const callback=take\nconst out=callback(small)\n"
            "const choice:%s=true?small:0\nconst generated=fn()->%s{return small}\n",
            from->name, to->name, from->name, to->name, to->name, to->name, to->name, to->name, to->name);
        CHECK(length > 0 && (size_t) length < sizeof(text)); write_source(root, text);
        XrXirArtifact *artifact = NULL;
        CHECK(xr_xir_source_check(request, &artifact, NULL) == XR_XIR_OK && artifact);
        const XrXirModule *module = xr_xir_artifact_module(artifact);
        unsigned conversions = 0;
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            for (uint32_t i = 0; i < function->instruction_count; ++i) {
                const XrXirInstruction *op = &function->instructions[i];
                if (op->op != XR_XIR_CONVERT_INT) continue;
                CHECK(op->type == to->type); ++conversions;
                uint32_t id = op->args[0];
                XrXirType input = id < function->parameter_count ? function->parameters[id] :
                    function->instructions[id - function->parameter_count].type;
                CHECK(input == from->type);
            }
        }
        CHECK(conversions == 7); ++cases; xr_xir_artifact_free(artifact);
    }
    CHECK(cases == 12);
    puts("Integer source contexts: 8 boundary types and 12 widening pairs across 7 sites");
}
#endif
