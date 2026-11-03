/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_signal_frontier.inc.c - Sealed promise lookup for actual frames
 *
 * KEY CONCEPT:
 *   A copied entry table carries no promises. Its actual Instance generation,
 *   code entry and complete parameter list must match the retained Program.
 */
static bool instance_signal_entry_equal(const XrXirCallEntry *actual,const XrXirCallEntry *sealed) {
    if (actual->abi_version!=sealed->abi_version || actual->parameter_count!=sealed->parameter_count ||
        actual->result!=sealed->result || actual->state_bytes!=sealed->state_bytes ||
        actual->resume!=sealed->resume || actual->release!=sealed->release ||
        actual->environment!=sealed->environment || actual->flags!=sealed->flags ||
        actual->cleanup_owner!=sealed->cleanup_owner) return false;
    if (actual->parameter_count && (!actual->parameters || !sealed->parameters)) return false;
    for (uint32_t p=0;p<actual->parameter_count;++p)
        if (actual->parameters[p]!=sealed->parameters[p]) return false;
    return true;
}
XR_FUNC bool xir_instance_signal_frame(const void *pointer,const XrXirCall *call,
    const XrXirValueAdmission *admission,const XrXirCallMetadata *metadata,
    uint32_t entry,bool *no_suspend) {
    if (!pointer || !call || !admission || admission->function!=admit_function_binding ||
        !metadata || !no_suspend || metadata->cell_role!=instance_cell_role) return false;
    XrXirInstance *root=(XrXirInstance *)pointer;
    if (!instance_owner_current(root) || instance_root(root)!=root || admission->domain!=root->domain ||
        root->stopping || instance_busy(root)) return false;
    XrXirInstance *active=instance_current(root),*selected=NULL;
    if (!active || active->call!=call || active->stopping || active->state!=XR_XIR_INSTANCE_READY ||
        instance_busy(active)) return false;
    if (metadata->admission_context==root) selected=root;
    else if (metadata->admission_context==active) selected=active;
    else for (XrXirInstance *state=root->generation_next;state;state=state->generation_next)
        if (metadata->admission_context==state) {selected=state;break;}
    const XrXirProgram *program=selected?selected->program:NULL;
    const XrXirProgramPermissions *permissions=program?program->permissions:NULL;
    const XrXirArtifact *artifact=permissions && permissions->input?permissions->input->artifact:NULL;
    if (!selected || instance_root(selected)!=root || selected->domain!=root->domain ||
        !program || !artifact || metadata->cell_context!=program || metadata->arena!=program->arena ||
        !metadata->entries || entry>=metadata->entry_count || !program->declarations ||
        artifact->module.stage!=XR_XIR_LOWERED || artifact->module.function_count!=program->entry_count ||
        artifact->program_final!=permissions->effects || artifact->layouts!=permissions->layouts ||
        (metadata->entry_count!=program->entry_count &&
         (uint64_t)metadata->entry_count!=(uint64_t)program->entry_count+1u)) return false;
    const XrXirCallEntry *sealed=NULL;
    if (entry<program->entry_count) sealed=&program->entries[entry];
    else {
        /* This sole extra entry belongs to the real current Instance relay,
         * not a function declaration or a shape-compatible host callback. */
        if (selected!=active || !selected->entries || entry!=program->entry_count) return false;
        sealed=&selected->entries[entry];
        if (sealed->resume!=initialization_resume || sealed->release!=initialization_release)
            return false;
    }
    if (!instance_signal_entry_equal(&metadata->entries[entry],sealed)) return false;
    *no_suspend=entry<program->entry_count &&
        (program->declarations->functions[entry].promises&XR_XIR_FUNCTION_NO_SUSPEND)!=0;
    return true;
}
