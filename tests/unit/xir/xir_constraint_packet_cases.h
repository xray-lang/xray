/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_constraint_packet_cases.h - Constraint packet ownership and forged requirements
 */
#ifndef XIR_CONSTRAINT_PACKET_CASES_H
#define XIR_CONSTRAINT_PACKET_CASES_H
#include "xir_constraint_packet_fixture.h"
static void constraint_record_equality_cases(void) {
    XrXirConstraint empty = {0};
    XrXirInterfaceDeclaration declaration = {{"alpha",5},{"Base",4},1,&empty,1,NULL,0,NULL,0};
    XrXirInterfaceDeclaration other_declaration = declaration;
    XrXirInterfaceTable from_table = {&declaration,1}, to_table = {&other_declaration,1};
    XrXirTypeNode from_node = {0}, to_nodes[2] = {0};
    from_node.kind = XR_XIR_TYPE_CALLABLE; from_node.parameter_span = 1;
    from_node.result = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    to_nodes[0].kind = XR_XIR_TYPE_ARRAY; to_nodes[0].element = XR_XIR_I64;
    to_nodes[1] = from_node;
    XrXirTypes from_types = {&from_node,1,NULL,&from_table}, to_types = {to_nodes,2,NULL,&to_table};
    XrXirType from_arg = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType to_arg = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
    XrXirInterfaceApplication from_app = {0,&from_arg,1}, to_app = {0,&to_arg,1};
    XrXirConstraint from = {0,&from_app,1}, to = {0,&to_app,1};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_constraint_records_match(&from_types,from,&to_types,to,1,&budget) == XR_XIR_OK);
    to_nodes[1].result = XR_XIR_I64; to_nodes[1].parameter_span = 0; budget = xr_xir_default_budget();
    CHECK(xr_xir_constraint_records_match(&from_types,from,&to_types,to,1,&budget) == XR_XIR_BAD_TYPE);
    to_nodes[1] = from_node; other_declaration.name = (XrXirLiteral){"Else",4}; budget = xr_xir_default_budget();
    CHECK(xr_xir_constraint_records_match(&from_types,from,&to_types,to,1,&budget) == XR_XIR_BAD_TYPE);
    other_declaration = declaration; to.markers = XR_XIR_CONSTRAINT_SENDABLE; budget = xr_xir_default_budget();
    CHECK(xr_xir_constraint_records_match(&from_types,from,&to_types,to,1,&budget) == XR_XIR_BAD_TYPE);
}
static void constraint_packet_cases(void) {
    constraint_record_equality_cases();
    for (unsigned mixed = 0; mixed < 2; ++mixed) {
        uint32_t markers = mixed ? XR_XIR_CONSTRAINT_SENDABLE : 0;
        XrXirArtifact *source = constraint_packet_fixture(markers), *decoded = NULL;
        XrXirCheckedPacket packet = {0}, second = {0};
        constraint_packet_owned(source,markers);
        CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
        xr_xir_artifact_free(source);
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
        CHECK(xr_xir_checked_write(decoded,NULL,&second,NULL) == XR_XIR_OK);
        CHECK(packet.length == second.length && !memcmp(packet.bytes,second.bytes,packet.length));
        xr_xir_checked_packet_free(&second);
        uint8_t record[20] = {0}; put32(record,markers); put32(record + 4,1);
        put32(record + 12,1); put32(record + 16,XR_XIR_TYPE_PARAMETER_BASE);
        unsigned found = 0;
        for (size_t at = 64; at + sizeof(record) <= packet.length; ++at) {
            if (memcmp(packet.bytes + at,record,sizeof(record))) continue;
            ++found;
            const unsigned offsets[] = {0,4,8,12,16};
            const uint32_t attacks[] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_TYPE_PARAMETER_BASE + 1};
            for (unsigned i = 0; i < 5; ++i) {
                put32(packet.bytes + at + offsets[i],attacks[i]); digest_packet(&packet);
                rejected(packet.bytes,packet.length);
                memcpy(packet.bytes + at,record,sizeof(record)); digest_packet(&packet);
            }
        }
        CHECK(found == 3);
        memset(packet.bytes,0xcc,packet.length); xr_xir_checked_packet_free(&packet);
        CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK);
        constraint_packet_owned(decoded,markers); xr_xir_artifact_free(decoded);
    }
}
#endif // XIR_CONSTRAINT_PACKET_CASES_H
