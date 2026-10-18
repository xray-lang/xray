/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_lifecycle_native_producer.h - Authentic generated lifecycle specifications
 */
#ifndef XIR_CELL_LIFECYCLE_NATIVE_PRODUCER_H
#define XIR_CELL_LIFECYCLE_NATIVE_PRODUCER_H
#include "xir/xxir_emit_c.h"

typedef struct CellLifeEmission {
    SourceFixtureOwner owner;
    XrXirCSource source;
    uint32_t entries[CELL_LIFE_ENTRIES], count;
    uint8_t *leaves;
} CellLifeEmission;

static bool cell_life_leaf_name(const XrXirFunction *function) {
    const char *const names[] = {"leaf", "throwing", "panicking", "paused"};
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); ++n) {
        size_t length = strlen(names[n]);
        if (function->name_length == length && !memcmp(function->name, names[n], length)) return true;
    }
    return false;
}
static void cell_life_emit(unsigned fixture, CellLifeEmission *output) {
    CHECK(fixture < 4 && !output->owner.context.resources && !output->source.text);
    source_fixture_owner_new(&output->owner);
    XrXirArtifact *lowered = NULL;
    CHECK(cell_source_lower(&output->owner.context, cell_life_fixtures[fixture], &lowered, false) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module && module->declarations && module->declarations->functions);
    output->count = module->function_count;
    for (unsigned i = 0; i < CELL_LIFE_ENTRIES; ++i)
        output->entries[i] = cell_source_entry(module, cell_life_entry_names[i], true);
    void *memory = NULL;
    CHECK(xr_compile_resources_calloc(output->owner.context.resources, output->count, 1, &memory) == XR_COMPILE_RESOURCE_OK);
    output->leaves = memory;
    unsigned leaves = 0;
    for (uint32_t f = 0; f < output->count; ++f) {
        CHECK(xr_compile_resources_work(output->owner.context.resources, 1) == XR_COMPILE_RESOURCE_OK);
        output->leaves[f] = (uint8_t)cell_life_leaf_name(&module->functions[f]);
        leaves += output->leaves[f];
    }
    CHECK(leaves == 4);
    char symbol[64];
    CHECK(snprintf(symbol, sizeof(symbol), "cell_life_generated_%u", fixture) > 0);
    CHECK(xr_xir_compile_emit_c(lowered, symbol, 4194304, &output->source) == XR_XIR_OK);
    CHECK(output->source.text && output->source.length && !output->source.text[output->source.length]);
    CHECK(!strstr(output->source.text, "({"));
    xr_xir_compile_artifact_free(lowered);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
}
static int cell_life_write_c(char **argv) {
    _Static_assert(sizeof(cell_life_fixtures)/sizeof(cell_life_fixtures[0]) == 4,
        "the complete Source lifecycle matrix has four fixtures");
    _Static_assert(CELL_LIFE_ENTRIES == 6, "generated lifecycle entry tables preserve all six exports");
    for (unsigned i = 2; i < 7; ++i) {
        CHECK(argv[i][0]);
        for (unsigned prior = 2; prior < i; ++prior) CHECK(strcmp(argv[i], argv[prior]));
    }
    CellLifeEmission outputs[4] = {0};
    for (unsigned i = 0; i < 4; ++i) cell_life_emit(i, &outputs[i]);
    for (unsigned i = 0; i < 4; ++i) {
        FILE *file = fopen(argv[i + 2], "wb"); CHECK(file);
        CHECK(fwrite(outputs[i].source.text, 1, outputs[i].source.length, file) == outputs[i].source.length && !fclose(file));
    }
    FILE *header = fopen(argv[6], "wb"); CHECK(header);
    CHECK(fputs("/* Source-derived entries; the producer and all artifacts are dead. */\n"
        "typedef struct CellLifeGeneratedFixture { const XrXirProgramSpec *program; "
        "uint32_t entries[6]; uint32_t count; const uint8_t *leaves; } CellLifeGeneratedFixture;\n", header) >= 0);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(fprintf(header, "XR_DATA const XrXirProgramSpec cell_life_generated_%u_program;\n"
            "static const uint8_t cell_life_generated_%u_leaves[%u]={", i, i, outputs[i].count) > 0);
        for (uint32_t f = 0; f < outputs[i].count; ++f)
            CHECK(fprintf(header, "%s%uu", f ? "," : "", (unsigned)outputs[i].leaves[f]) > 0);
        CHECK(fputs("};\n", header) >= 0);
    }
    CHECK(fputs("static const CellLifeGeneratedFixture cell_life_generated_fixtures[4]={\n", header) >= 0);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(fprintf(header, "{&cell_life_generated_%u_program,{", i) > 0);
        for (unsigned e = 0; e < CELL_LIFE_ENTRIES; ++e)
            CHECK(fprintf(header, "%s%uu", e ? "," : "", outputs[i].entries[e]) > 0);
        CHECK(fprintf(header, "},%uu,cell_life_generated_%u_leaves},\n", outputs[i].count, i) > 0);
    }
    CHECK(fputs("};\n", header) >= 0 && !fclose(header));
    for (unsigned i = 0; i < 4; ++i) {
        printf("CELL_LIFE_GENERATE fixture=%s functions=%u Cbytes=%zu W1W4=1 runtimeExecution=NOT_RUN\n",
            cell_life_fixtures[i], outputs[i].count, outputs[i].source.length);
        xr_compile_resources_free(outputs[i].leaves);
        xr_xir_compile_c_source_free(&outputs[i].source);
        source_fixture_owner_free(&outputs[i].owner);
    }
    cell_life_zero(); return 0;
}
#endif // XIR_CELL_LIFECYCLE_NATIVE_PRODUCER_H
