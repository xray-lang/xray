/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_derivation_codec.inc.c - Canonical derivation policy and binding fields
 *
 * KEY CONCEPT:
 *   Partial decoded owners attach every nested allocation before publication.
 */
#include "xxir_derivation_internal.h"

static void checked_derivation_origin(CheckedCursor *c,XrXirGeneratedOrigin *origin) {
    origin->module.bytes=checked_blob(c,origin->module.bytes,&origin->module.length);
    origin->file.bytes=checked_blob(c,origin->file.bytes,&origin->file.length);
    origin->field_path.bytes=checked_blob(c,origin->field_path.bytes,&origin->field_path.length);
    origin->line=checked_u32(c,origin->line);
    origin->column=checked_u32(c,origin->column);
}
static void checked_derivation_policy(CheckedCursor *c,XrXirDerivationPolicy *policy) {
    policy->grantor_module=checked_u32(c,policy->grantor_module);
    policy->target=(XrXirType)checked_u32(c,(uint32_t)policy->target);
    checked_application(c,&policy->protocol);
    policy->generator=checked_u32(c,policy->generator);
    policy->constructor=checked_u32(c,policy->constructor);
    uint32_t count=checked_count(c,policy->member_count,&c->structural.parameters,8);
    XrXirDerivationMember *members=checked_array(c,policy->members,count,sizeof(*members),8);
    policy->members=members;policy->member_count=members?count:0;
    for (uint32_t i=0; i<policy->member_count && c->status==XR_XIR_OK; ++i) {
        XrXirDerivationMember member=members[i];
        member.field=checked_u32(c,member.field);
        member.operations=checked_u32(c,member.operations);
        if (c->reading) members[i]=member;
    }
    checked_derivation_origin(c,&policy->origin);
}
static void checked_derivation_binding(CheckedCursor *c,XrXirDerivationGrant *binding,
    const XrXirModule *module) {
    binding->function=checked_u32(c,binding->function);
    binding->policy=checked_u32(c,binding->policy);
    binding->source_row=checked_u32(c,binding->source_row);
    binding->root=checked_u32(c,binding->root);
    binding->target=(XrXirType)checked_u32(c,(uint32_t)binding->target);
    checked_derivation_origin(c,&binding->origins.entry);
    uint32_t count=checked_u32(c,binding->origins.instruction_count);
    if (c->status==XR_XIR_OK && (binding->function>=module->function_count ||
        count!=module->functions[binding->function].instruction_count)) {
        c->status=XR_XIR_BAD_STRUCTURE;
    }
    XrXirGeneratedOrigin *origins=checked_array(c,binding->origins.instructions,
        count,sizeof(*origins),20);
    binding->origins.instructions=origins;binding->origins.instruction_count=origins?count:0;
    for (uint32_t i=0; i<binding->origins.instruction_count && c->status==XR_XIR_OK; ++i) {
        XrXirGeneratedOrigin origin=origins[i];
        checked_derivation_origin(c,&origin);
        if (c->reading) origins[i]=origin;
    }
}
static XrXirDerivationTable *checked_derivation_table_new(CheckedCursor *c,
    XrXirDeclarations *declarations) {
    XrXirDerivationTable *owned=checked_array(c,NULL,1,sizeof(*owned),4);
    declarations->derivations=owned;
    return owned;
}
static void checked_derivations(CheckedCursor *c,XrXirDeclarations *declarations,
    const XrXirModule *module) {
    const XrXirDerivationTable *source=declarations->derivations;
    uint32_t policies=checked_count(c,source?source->policy_count:0,
        &c->structural.parameters,32);
    if (c->status!=XR_XIR_OK) return;
    XrXirDerivationTable *owned=NULL;
    if (c->reading && policies) {
        owned=checked_derivation_table_new(c,declarations);
        if (!owned) return;
    }
    XrXirDerivationPolicy *policy_rows=checked_array(c,source?source->policies:NULL,
        policies,sizeof(*policy_rows),32);
    if (owned) { owned->policies=policy_rows;owned->policy_count=policy_rows?policies:0; }
    for (uint32_t i=0; policy_rows && i<policies && c->status==XR_XIR_OK; ++i) {
        XrXirDerivationPolicy policy=policy_rows[i];
        checked_derivation_policy(c,&policy);
        if (c->reading) policy_rows[i]=policy;
    }
    uint32_t count=checked_u32(c,source?source->count:0);
    if (c->status!=XR_XIR_OK) return;
    if (count>module->function_count) { c->status=XR_XIR_BAD_STRUCTURE;return; }
    if (c->reading && count && !owned) {
        owned=checked_derivation_table_new(c,declarations);
        if (!owned) return;
    }
    XrXirDerivationGrant *bindings=checked_array(c,source?source->records:NULL,
        count,sizeof(*bindings),44);
    if (owned) { owned->records=bindings;owned->count=bindings?count:0; }
    for (uint32_t i=0; bindings && i<count && c->status==XR_XIR_OK; ++i) {
        XrXirDerivationGrant binding=bindings[i];
        checked_derivation_binding(c,&binding,module);
        if (c->reading) bindings[i]=binding;
    }
    if (c->reading && !owned) declarations->derivations=NULL;
}
