/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_comptime_pending.inc.c - Atomic owning observations of unresolved keys
 *
 * KEY CONCEPT:
 *   Every borrow verifies current Source bytes and repeats actual discovery.
 *   Owned type descriptions preserve lifetime without executable authority.
 */
#include "xxir_checked.h"
struct XirComptimePending {
    XrXirCompileContext context;
    XrXirArtifact *definition;
    XrXirCheckedPacket packet;
    XrXirType *arguments;
    XrXirComptimeSite site;
    uint32_t count;
};
static void pending_point_free(SpecPendingPoint *point) {
    xr_xir_compile_types_free(point->types);xr_compile_resources_free(point->arguments);
    *point=(SpecPendingPoint){0};
}
XR_FUNC void xr_xir_compile_comptime_pending_free(XirComptimePending *owner) {
    if (!owner) return;
    xr_xir_compile_artifact_free(owner->definition);xr_xir_compile_checked_packet_free(&owner->packet);
    xr_compile_resources_free(owner->arguments);xr_compile_resources_free(owner);
}
static XrXirStatus pending_status(XrXirDiagnostic *diagnostic,XrXirStatus status) {
    if (diagnostic) diagnostic->status=status;
    return status;
}
static XrXirStatus pending_definition_receive(XirComptimePending *owner,const SpecPendingPoint *point,
    XrXirDiagnostic *diagnostic) {
    /* The complete Source owns every new descriptor. Its current packet covers
     * the whole pool, including declarations not reached by this argument. */
    XrXirModule module=owner->definition->module;module.types=point->types;
    XrXirArtifact *received=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_recheck_v2(&owner->context,&module,owner->definition->construction,&received,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(received,&packet,diagnostic);
    if (status==XR_XIR_OK) {
        xr_xir_compile_artifact_free(owner->definition);xr_xir_compile_checked_packet_free(&owner->packet);
        owner->definition=received;owner->packet=packet;received=NULL;packet=(XrXirCheckedPacket){0};
    }
    xr_xir_compile_artifact_free(received);xr_xir_compile_checked_packet_free(&packet);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_comptime_pending(const XrXirArtifact *definition,
    XirComptimePending **output,XrXirDiagnostic *diagnostic) {
    if (!definition || !output || *output || !xir_compile_context_valid(&definition->context))
        return pending_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    if (definition->module.stage!=XR_XIR_CHECKED || definition->module.linkage_kind!=XR_XIR_PROGRAM ||
        xir_effect_evidence_is_closed(&definition->module)) return pending_status(diagnostic,XR_XIR_BAD_STAGE);
    XrXirStatus status=XR_XIR_OK;XirComptimePending *owner=xir_compile_calloc(&definition->context,1,sizeof(*owner),&status);
    if (!owner) return pending_status(diagnostic,status);
    owner->context=definition->context;SpecPendingPoint point={0};XrXirArtifact *unexpected=NULL;
    status=xr_xir_compile_checked_write(definition,&owner->packet,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(&owner->context,owner->packet.bytes,owner->packet.length,&owner->definition,diagnostic);
    if (status==XR_XIR_OK && owner->definition->construction->comptime.site_count)
        status=spec_run(owner->definition,&unexpected,diagnostic,&point,NULL,NULL);
    if (unexpected) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && point.found) status=pending_definition_receive(owner,&point,diagnostic);
    if (status==XR_XIR_OK && point.found) {
        {
            owner->site=point.site;owner->count=point.count;owner->arguments=point.arguments;
            point.arguments=NULL;*output=owner;owner=NULL;
        }
    }
    xr_xir_compile_artifact_free(unexpected);pending_point_free(&point);xr_xir_compile_comptime_pending_free(owner);
    return pending_status(diagnostic,status);
}
XR_FUNC XrXirStatus xr_xir_compile_comptime_pending_view(const XirComptimePending *owner,
    XirComptimePendingView *output,XrXirDiagnostic *diagnostic) {
    if (!owner || !output || output->definition || output->types || output->arguments || output->argument_count ||
        output->site.caller || output->site.instruction || output->site.helper ||
        !xir_compile_context_valid(&owner->context) || !owner->definition ||
        owner->definition->context.resources!=owner->context.resources) return pending_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    XrXirCheckedPacket current={0};SpecPendingPoint point={0};XrXirArtifact *unexpected=NULL;
    XrXirStatus status=xr_xir_compile_checked_write(owner->definition,&current,diagnostic);
    if (status==XR_XIR_OK) {
        if (current.length!=owner->packet.length || !owner->packet.bytes || memcmp(current.bytes,owner->packet.bytes,current.length)) status=XR_XIR_BAD_STRUCTURE;
    }
    if (status==XR_XIR_OK) status=spec_run(owner->definition,&unexpected,diagnostic,&point,NULL,NULL);
    if (status==XR_XIR_OK && (!point.found || point.count!=owner->count || point.site.caller!=owner->site.caller ||
        point.site.instruction!=owner->site.instruction || point.site.helper!=owner->site.helper || unexpected)) status=XR_XIR_BAD_STRUCTURE;
    for (uint32_t a=0;status==XR_XIR_OK && a<owner->count;++a)
        status=xr_xir_compile_type_substitution_matches_between(&owner->context,point.types,owner->definition->module.types,NULL,0,point.arguments[a],owner->arguments[a]);

    if (status==XR_XIR_OK) *output=(XirComptimePendingView){owner->definition,owner->definition->module.types,owner->site,owner->arguments,owner->count};
    xr_xir_compile_artifact_free(unexpected);pending_point_free(&point);xr_xir_compile_checked_packet_free(&current);
    return pending_status(diagnostic,status);
}
XR_FUNC XrXirStatus xr_xir_compile_comptime_pending_specialize(const XirComptimePending *owner,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    if (!output || *output) return pending_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    XirComptimePendingView view={0};
    XrXirStatus status=xr_xir_compile_comptime_pending_view(owner,&view,diagnostic);
    XrXirArtifact *received=NULL;XirEvaluationRoot root={0};
    if (status==XR_XIR_OK) status=spec_run(view.definition,&received,diagnostic,NULL,&root,NULL);
    if (status==XR_XIR_OK) {
        const XrXirProvenance *proof=received->module.provenance;
        const XrXirOrigin *caller=&proof->origins[root.caller],*callee=&proof->origins[root.callee];
        if (caller->function!=view.site.caller || root.instruction!=view.site.instruction ||
            callee->function!=view.site.helper || callee->argument_count!=view.argument_count) status=XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=0;status==XR_XIR_OK && a<view.argument_count;++a)
            status=xr_xir_compile_type_substitution_matches_between(&owner->context,view.types,received->module.types,
                NULL,0,view.arguments[a],callee->arguments[a]);
    }
    if (status==XR_XIR_OK) { *output=received;received=NULL; }
    xr_xir_compile_artifact_free(received);return pending_status(diagnostic,status);
}
