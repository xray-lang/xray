/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_class_metadata.h - Explicit class promises survive owned products
 */
#ifndef XIR_TASK_CLASS_METADATA_H
#define XIR_TASK_CLASS_METADATA_H
#include "xir/xxir_constraint_proof.h"
static void task_class_metadata(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirNominalTable *table=module->types->nominals;
    CHECK(table && table->count && table->count<=128);
    XrXirNominalTable bad_table=*table;XrXirTypes bad_types=*module->types;
    XrXirNominalDeclaration declarations[128];XrXirNominalIdentity identities[128];
    XrXirNominalField fields[2];XrXirNominalFieldIdentity identity_fields[2];
    uint32_t payload=UINT32_MAX;
    for (uint32_t n=0;n<table->count;++n) {
        XrXirLiteral name=table->declarations?table->declarations[n].name:table->identities[n].name;
        if (name.length==7 && !memcmp(name.bytes,"Payload",7)) payload=n;
    }
    CHECK(payload!=UINT32_MAX);
    if (table->declarations) {
        memcpy(declarations,table->declarations,table->count*sizeof(*declarations));
        CHECK(declarations[payload].flags & XR_XIR_NOMINAL_SENDABLE);
        CHECK(declarations[payload].field_count==2);
        memcpy(fields,declarations[payload].fields,sizeof(fields));
        CHECK(!(fields[0].flags & XR_XIR_FIELD_MUTABLE) && !(fields[1].flags & XR_XIR_FIELD_MUTABLE));
        fields[0].flags|=XR_XIR_FIELD_MUTABLE;declarations[payload].fields=fields;
        bad_table.declarations=declarations;
    } else {
        memcpy(identities,table->identities,table->count*sizeof(*identities));
        CHECK(identities[payload].flags & XR_XIR_NOMINAL_SENDABLE);
        CHECK(identities[payload].field_count==2);
        memcpy(identity_fields,identities[payload].fields,sizeof(identity_fields));
        CHECK(!(identity_fields[0].flags & XR_XIR_FIELD_MUTABLE) && !(identity_fields[1].flags & XR_XIR_FIELD_MUTABLE));
        identity_fields[0].flags|=XR_XIR_FIELD_MUTABLE;identities[payload].fields=identity_fields;
        bad_table.identities=identities;
    }
    bad_types.nominals=&bad_table;
    CHECK(xr_xir_compile_types_structure_verify(context,&bad_types)==XR_XIR_BAD_TYPE);
}
#endif // XIR_TASK_CLASS_METADATA_H
