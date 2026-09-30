/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_metadata_fixture.h - Independent Array operation and place graphs
 *
 * KEY CONCEPT:
 *   Metadata tests do not depend on source lowering or runtime Array code.
 */
#ifndef XIR_ARRAY_METADATA_FIXTURE_H
#define XIR_ARRAY_METADATA_FIXTURE_H
#include "xir/xxir.h"

typedef struct XirArrayMetadataFixture {
    XrXirTypeNode nodes[3];
    XrXirTypes types;
    XrXirInstruction init[3], ops[18], worker[2];
    XrXirBlock blocks[3];
    XrXirFunction functions[3];
    XrXirType worker_parameter;
    uint32_t operands[6];
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[3];
    XrXirSlot slot;
    XrXirDeclarations declarations;
    XrXirModule module;
} XirArrayMetadataFixture;

static void xir_array_metadata_init(XirArrayMetadataFixture *f) {
    memset(f, 0, sizeof(*f));
    XrXirType array = (XrXirType) 256, cell = (XrXirType) 257;
    f->nodes[0] = (XrXirTypeNode) {XR_XIR_TYPE_ARRAY, XR_XIR_I64, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    f->nodes[1] = (XrXirTypeNode) {XR_XIR_TYPE_CELL, array, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    f->nodes[2] = (XrXirTypeNode) {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, NULL, 0, XR_XIR_I64, 0, 0, {0}};
    f->types = (XrXirTypes) {f->nodes, 3, NULL, NULL};
    f->init[0] = (XrXirInstruction) {XR_XIR_ARRAY_NEW, array, {0}, {0}, 0, {0}};
    f->init[1] = (XrXirInstruction) {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    f->init[2] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    f->ops[0] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 42, {0}};
    f->ops[1] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 43, {0}};
    f->ops[2] = (XrXirInstruction) {XR_XIR_ARRAY_NEW, array, {0, 2}, {0}, 0, {0}};
    f->ops[3] = (XrXirInstruction) {XR_XIR_LOCAL_NEW, array, {2}, {0}, 0, {0}};
    f->ops[4] = (XrXirInstruction) {XR_XIR_CELL_NEW, cell, {2}, {0}, 0, {0}};
    f->ops[5] = (XrXirInstruction) {XR_XIR_CELL_PLACE, array, {4}, {0}, 0, {0}};
    f->ops[6] = (XrXirInstruction) {XR_XIR_SLOT_PLACE, array, {0}, {0}, 0, {0}};
    f->ops[7] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}};
    f->ops[8] = (XrXirInstruction) {XR_XIR_ARRAY_GET, XR_XIR_I64, {5, 7}, {0}, 0, {0}};
    f->ops[9] = (XrXirInstruction) {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {2, 3}, {0}, 0, {0}};
    f->ops[10] = (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {5, 8}, {0}, 0, {0}};
    f->ops[11] = (XrXirInstruction) {XR_XIR_ARRAY_GET, XR_XIR_I64, {3, 7}, {0}, 0, {0}};
    f->ops[12] = (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {6}, {0}, 0, {0}};
    f->ops[13] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {12}, {0}, 0, {0}};
    f->operands[0] = 0; f->operands[1] = 1;
    f->operands[2] = 3; f->operands[3] = 7; f->operands[4] = 1;
    f->worker_parameter = array;
    f->worker[0] = (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {0}, {0}, 0, {0}};
    f->worker[1] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}};
    f->blocks[0] = (XrXirBlock) {0, 3, 0, 0};
    f->blocks[1] = (XrXirBlock) {0, 14, 0, 0};
    f->blocks[2] = (XrXirBlock) {0, 2, 0, 0};
    f->functions[0] = (XrXirFunction) {"init", 4, NULL, 0, XR_XIR_UNIT, &f->blocks[0], 1, f->init, 3, NULL, 0};
    f->functions[1] = (XrXirFunction) {"entry", 5, NULL, 0, XR_XIR_I64, &f->blocks[1], 1, f->ops, 14, f->operands, 5};
    f->functions[2] = (XrXirFunction) {"read", 4, &f->worker_parameter, 1, XR_XIR_I64, &f->blocks[2], 1, f->worker, 2, NULL, 0};
    f->source = (XrXirSourceModule) {"app", 3, NULL, 0, 0};
    f->slot = (XrXirSlot) {0, array, 1};
    f->declarations = (XrXirDeclarations) {&f->source, 1, f->identities, &f->slot, 1, NULL, 0, 0, 1};
    f->module = (XrXirModule) {XR_XIR_BUILT, f->functions, 3, &f->declarations, NULL, &f->types, NULL};
}
#endif // XIR_ARRAY_METADATA_FIXTURE_H
