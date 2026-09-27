/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_emit_program.c - Emit compiler-owned native program descriptors
 *
 * KEY CONCEPT:
 *   Native tests consume emitted code and data after all compiler owners die.
 */
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_program_fixture.h"
#include "xir_capture_fixture.h"
#include "xir_array_program_fixture.h"
#include "xir_nominal_checked_fixture.h"
#include "xir_nominal_generic_fixture.h"
#include "xir_nominal_expression_fixture.h"
#include "xir_nominal_chain_fixture.h"
#include "xir_nominal_transport_fixture.h"
#include "xir_struct_ops_fixture.h"
#include "xir_struct_set_fixture.h"
int main(int argc, char **argv) {
    FILE *file = argc == 2 ? fopen(argv[1], "wb") : NULL;
    CHECK(argc == 1 || (argc == 2 && file));
    XrXirArtifact *struct_set = struct_set_lowered();
    XrXirCSource set_source = {0};
    CHECK(xr_xir_emit_c(struct_set,"struct_set",200000,&set_source) == XR_XIR_OK);
    xr_xir_artifact_free(struct_set);
    CHECK(!strstr(set_source.text,"xr_xir_vm") && !strstr(set_source.text,"({"));
    if (file) CHECK(fwrite(set_source.text,1,set_source.length,file) == set_source.length);
    xr_xir_c_source_free(&set_source);
    XrXirArtifact *struct_ops = struct_ops_lowered();
    XrXirCSource struct_source = {0};
    CHECK(xr_xir_emit_c(struct_ops,"struct_ops",200000,&struct_source) == XR_XIR_OK);
    xr_xir_artifact_free(struct_ops);
    CHECK(!strstr(struct_source.text,"xr_xir_vm") && !strstr(struct_source.text,"({"));
    if (file) CHECK(fwrite(struct_source.text,1,struct_source.length,file) == struct_source.length);
    xr_xir_c_source_free(&struct_source);
    XrXirArtifact *transport = nominal_transport_fixture();
    XrXirCSource transport_source = {0};
    CHECK(xr_xir_emit_c(transport,"nominal_transport",200000,&transport_source) == XR_XIR_OK);
    xr_xir_artifact_free(transport);
    CHECK(!strstr(transport_source.text,"xr_xir_vm") && !strstr(transport_source.text,"({"));
    if (file) CHECK(fwrite(transport_source.text,1,transport_source.length,file) == transport_source.length);
    xr_xir_c_source_free(&transport_source);
    XrXirArtifact *combined = nominal_generic_lowered();
    XrXirCSource combined_source = {0};
    CHECK(xr_xir_emit_c(combined, "nominal_generic", 200000, &combined_source) == XR_XIR_OK);
    xr_xir_artifact_free(combined);
    CHECK(!strstr(combined_source.text, "xr_xir_vm") && !strstr(combined_source.text, "({"));
    if (file) CHECK(fwrite(combined_source.text, 1, combined_source.length, file) == combined_source.length);
    xr_xir_c_source_free(&combined_source);
    XrXirArtifact *expressions = nominal_expression_lowered();
    XrXirCSource expression_source = {0};
    CHECK(xr_xir_emit_c(expressions, "nominal_expression", 200000, &expression_source) == XR_XIR_OK);
    xr_xir_artifact_free(expressions);
    CHECK(!strstr(expression_source.text, "xr_xir_vm") && !strstr(expression_source.text, "({"));
    if (file) CHECK(fwrite(expression_source.text, 1, expression_source.length, file) == expression_source.length);
    xr_xir_c_source_free(&expression_source);
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirArtifact *artifact = program_fixture(mode);
        char prefix[32]; CHECK(snprintf(prefix, sizeof(prefix), "program%u", mode) > 0);
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(artifact, prefix, 200000, &source) == XR_XIR_OK);
        xr_xir_artifact_free(artifact);
        CHECK(strstr(source.text, "XrXirProgramSpec") && strstr(source.text, "xr_xir_instance_slot_write"));
        CHECK(!strstr(source.text, "xr_xir_vm") && !strstr(source.text, "({"));
        if (file) CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        xr_xir_c_source_free(&source);
    }
    XrXirArtifact *captures = capture_fixture();
    XrXirCSource capture_source = {0};
    CHECK(xr_xir_emit_c(captures,"captures",200000,&capture_source) == XR_XIR_OK);
    xr_xir_artifact_free(captures);
    CHECK(!strstr(capture_source.text,"xr_xir_vm") && !strstr(capture_source.text,"({"));
    if (file) CHECK(fwrite(capture_source.text,1,capture_source.length,file) == capture_source.length);
    xr_xir_c_source_free(&capture_source);
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirArtifact *array = array_program_fixture(mode == 1, mode == 2);
        XrXirCSource source = {0}; char prefix[32];
        CHECK(snprintf(prefix,sizeof(prefix),"array_program%u",mode) > 0);
        CHECK(xr_xir_emit_c(array,prefix,200000,&source) == XR_XIR_OK);
        xr_xir_artifact_free(array);
        CHECK(!strstr(source.text,"xr_xir_vm") && !strstr(source.text,"({"));
        if (file) CHECK(fwrite(source.text,1,source.length,file) == source.length);
        xr_xir_c_source_free(&source);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirArtifact *nominal = mode == 2 ? nominal_chain_lowered() : nominal_lowered_fixture(mode ? 3 : 0);
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(nominal, mode == 2 ? "nominal_chain" : mode ? "nominal3" : "nominal0", 200000, &source) == XR_XIR_OK);
        xr_xir_artifact_free(nominal);
        CHECK(strstr(source.text, "XrXirNominalIdentity") && !strstr(source.text, "XrXirNominalDeclaration"));
        CHECK(!strstr(source.text, "xr_xir_vm") && !strstr(source.text, "({"));
        if (file) CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        xr_xir_c_source_free(&source);
    }
    if (file) CHECK(fclose(file) == 0);
    puts("Native program code and immutable declarations emitted");
    return 0;
}
