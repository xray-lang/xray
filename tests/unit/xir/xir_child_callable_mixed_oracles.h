/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_child_callable_mixed_oracles.h - Independent mixed body and owner evidence
 *
 * KEY CONCEPT:
 *   Observe authentic callback bodies without replacing their active environments.
 */
#ifndef XIR_CHILD_CALLABLE_MIXED_ORACLES_H
#define XIR_CHILD_CALLABLE_MIXED_ORACLES_H

enum { CM_VM, CM_NATIVE, CM_PROVIDERS };
typedef struct CmWitness {
    unsigned steps[CM_PROVIDERS][CN_FUNCTIONS];
    unsigned releases[CM_PROVIDERS][CN_FUNCTIONS];
    unsigned child_relay[CM_PROVIDERS], relay_pure[CM_PROVIDERS], awaits[CM_PROVIDERS];
    XrXirCall *child_call;
    XrXirValue captured;
} CmWitness;
typedef struct CmFixture {
    CnFixture base;
    XrXirArtifact *lowered;
    XrXirVmBinding vm_bindings[CN_FUNCTIONS];
    XrXirCallEntry vm_entries[CN_FUNCTIONS];
    const XrXirCallEntry *selected[CN_FUNCTIONS];
    unsigned providers[CN_FUNCTIONS];
    CmWitness witnesses[CN_INSTANCES];
    bool root_native;
} CmFixture;
static CmFixture *cm_observed;

static uint32_t cm_function(const XrXirCallView *view) {
    CHECK(cm_observed && view && view->environment);
    for (uint32_t i = 0; i < CN_FUNCTIONS; ++i) {
        if (view->environment != cm_observed->base.entries[i].environment) continue;
        const XrXirCallEntry *selected = cm_observed->selected[i];
        CHECK(selected && selected->resume && selected->release);
        if (cm_observed->providers[i] == CM_NATIVE) {
            const CnBinding *binding = view->environment;
            CHECK(binding == &cm_observed->base.bindings[i] && binding->function == i);
            CHECK(binding->original == &child_callable_native_entries[i] && selected == binding->original);
        } else {
            const XrXirVmBinding *binding = view->environment;
            CHECK(binding == &cm_observed->vm_bindings[i] && binding->function == i);
            CHECK(binding->artifact == cm_observed->lowered && selected == &cm_observed->vm_entries[i]);
            CHECK(selected->environment == binding);
        }
        return i;
    }
    CHECK(false);
    return UINT32_MAX;
}

static void cm_indirect(XrXirCallView *view, CnWitness *w, uint32_t function,
    const XrXirAction *action) {
    CHECK(function == CN_CHILD || function == CN_RELAY);
    CmFixture *f = cm_observed;
    CmWitness *m = &f->witnesses[w - f->base.witnesses];
    const unsigned provider = f->providers[function];
    const uint32_t target = function == CN_CHILD ? CN_RELAY : CN_PURE;
    CHECK(action->kind == XR_XIR_ACTION_CALL && action->callee == target);
    CHECK(!action->argument_count && action->value.type == CN_NONE && !action->value.reserved);
    CHECK(f->providers[target] != provider);
    CHECK(xr_xir_value_valid(&action->value));
    const XrXirFunctionBinding *binding = xr_xir_function_binding(&action->value);
    CHECK(binding && binding->entry == target && binding->capture_count == (function == CN_CHILD ? 1u : 0u));
    cn_resolve_expected(view, w, &action->value, XR_XIR_CALL_READY, target, false, xr_xir_call_admission(view));
    if (function == CN_CHILD) {
        CHECK(binding->captures && binding->captures[0].type == CN_NONE && !binding->captures[0].reserved);
        const XrXirFunctionBinding *capture = xr_xir_function_binding(&binding->captures[0]);
        CHECK(capture && capture->entry == CN_PURE && !capture->capture_count);
        cn_resolve_expected(view, w, &binding->captures[0], XR_XIR_CALL_READY, CN_PURE, false, xr_xir_call_admission(view));
        m->captured = binding->captures[0];
        CHECK(++m->child_relay[provider] == 1);
    } else {
        CHECK(!binding->captures && !memcmp(&action->value, &m->captured, sizeof(action->value)));
        CHECK(++m->relay_pure[provider] == 1);
    }
}

static XrXirAction cm_resume(XrXirCallView *view) {
    const uint32_t function = cm_function(view);
    CmFixture *f = cm_observed;
    CnWitness *w = cn_witness(view->instance);
    CmWitness *m = &f->witnesses[w - f->base.witnesses];
    const unsigned provider = f->providers[function];
    CHECK(xr_xir_call_admission(view));
    bool child = false;
    CHECK(xr_xir_task_executor_view_member(w->instance->executor, view, &child));
    ++w->calls[function];
    ++m->steps[provider][function];
    if (function == CN_FACTORY) { CHECK(!child); cn_materialize(view, w); }
    if (function == CN_ROOT) {
        CHECK(!child);
        if (!w->root_call) w->root_call = view->activation;
        CHECK(w->root_call == view->activation);
        w->saved_root = *view;
    }
    if (function == CN_CHILD || function == CN_RELAY || function == CN_PURE) {
        CHECK(child && w->root_call && view->activation != w->root_call);
        if (!m->child_call) { CHECK(function == CN_CHILD); m->child_call = view->activation; }
        CHECK(view->activation == m->child_call);
        ++w->child_callbacks;
        if (function == CN_CHILD) cn_child_probes(view, w);
        if (function == CN_RELAY) {
            CHECK(view->argument_count == 1 && view->arguments);
            CHECK(!memcmp(&view->arguments[0], &m->captured, sizeof(m->captured)));
        }
    }
    const void *environment = view->environment;
    XrXirAction action = f->selected[function]->resume(view);
    CHECK(view->environment == environment && xr_xir_call_admission(view));
    if (function == CN_ROOT && action.kind == XR_XIR_ACTION_AWAIT_TASK) {
        CHECK(++w->await_actions == 1 && ++m->awaits[provider] == 1);
        CHECK(action.value.type == CN_TASK && !action.value.reserved && xr_xir_value_valid(&action.value));
    }
    if ((function == CN_CHILD || function == CN_RELAY) && action.kind == XR_XIR_ACTION_CALL)
        cm_indirect(view, w, function, &action);
    if (function == CN_PURE && action.kind == XR_XIR_ACTION_RETURN)
        CHECK(action.value.type == XR_XIR_I64 && !action.value.reserved && action.value.payload == 42);
    return action;
}

static void cm_release(XrXirCallView *view, XrXirCallStatus reason) {
    const uint32_t function = cm_function(view);
    CmFixture *f = cm_observed;
    CnWitness *w = cn_witness(view->instance);
    CmWitness *m = &f->witnesses[w - f->base.witnesses];
    CHECK(!xr_xir_call_admission(view));
    cn_resolve_expected(view, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0, true, NULL);
    ++w->releases[function];
    ++m->releases[f->providers[function]][function];
    const void *environment = view->environment;
    f->selected[function]->release(view, reason);
    CHECK(view->environment == environment);
}

static void cm_code_drop(void *owner) {
    CmFixture *f = owner;
    CHECK(f == cm_observed && cn_observed == &f->base && f->lowered && !f->base.code_releases);
    xr_xir_compile_artifact_free(f->lowered);
    f->lowered = NULL;
    ++f->base.code_releases;
}

static void cm_layout_equal(const XrXirFunctionLayout *left, const XrXirFunctionLayout *right,
    uint32_t parameters) {
    CHECK(left->slot_count == right->slot_count && left->frame_bytes == right->frame_bytes);
    CHECK(left->owned_count == right->owned_count && left->outgoing_count == right->outgoing_count);
    CHECK(left->path_count == right->path_count);
    CHECK(left->result.size == right->result.size && left->result.alignment == right->result.alignment);
    for (uint32_t i = 0; i < left->slot_count; ++i) CHECK(left->offsets[i] == right->offsets[i]);
    for (uint32_t i = 0; i < left->owned_count; ++i) CHECK(left->owned_offsets[i] == right->owned_offsets[i]);
    for (uint32_t i = 0; i < parameters; ++i) {
        CHECK(left->parameters[i].size == right->parameters[i].size);
        CHECK(left->parameters[i].alignment == right->parameters[i].alignment);
    }
}

static void cm_build(CmFixture *f, bool root_native) {
    CHECK(!cm_observed && !cn_observed);
    cm_observed = f;
    cn_observed = &f->base;
    f->root_native = root_native;
    f->base.context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    const XrXirProgramSpec *native = &child_callable_native_program;
    CHECK(native->entry_count == CN_FUNCTIONS && native->entries == child_callable_native_entries);
    CHECK(native->declarations && native->declarations->entry_function == CN_ENTRY);
    CHECK(native->proof.bytes && native->proof.length && native->proof.identity && native->proof.layouts);
    XrXirArtifact *checked = NULL, *closed = NULL;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read(f->base.context, native->proof.bytes, native->proof.length,
        &checked, &diagnostic) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(closed, &packet, &diagnostic) == XR_XIR_OK);
    CHECK(packet.length == native->proof.length && !memcmp(packet.bytes, native->proof.bytes, packet.length));
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_lower(closed, &native->target, &f->lowered, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(f->lowered);
    CHECK(module && module->stage == XR_XIR_LOWERED && module->function_count == CN_FUNCTIONS);
    CHECK(module->declarations && module->declarations->entry_function == CN_ENTRY);
    CHECK(module->types && module->types->count == 5);
    const uint32_t advertised[4] = {2u, 4u, 8u, 12u};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(module->types->nodes[i + 1].kind == XR_XIR_TYPE_CALLABLE);
        CHECK(module->types->nodes[i + 1].flags == advertised[i]);
    }
    XrXirProgramProof proof = xr_xir_compile_program_proof(f->lowered);
    CHECK(proof.bytes && proof.identity && proof.layouts && proof.length == native->proof.length);
    CHECK(!memcmp(proof.bytes, native->proof.bytes, proof.length) && !memcmp(proof.identity, native->proof.identity, 32));
    const unsigned root_provider = root_native ? CM_NATIVE : CM_VM;
    for (uint32_t i = 0; i < CN_FUNCTIONS; ++i) {
        CHECK(xr_xir_compile_vm_bind(f->lowered, i, &f->vm_bindings[i], &f->vm_entries[i]) == XR_XIR_OK);
        cm_layout_equal(&proof.layouts[i], &native->proof.layouts[i], module->functions[i].parameter_count);
        f->providers[i] = root_provider;
        if (i == CN_CHILD || i == CN_PURE) f->providers[i] = 1u - root_provider;
        const XrXirCallEntry *selected = f->providers[i] == CM_NATIVE ? &native->entries[i] : &f->vm_entries[i];
        CHECK(native->entries[i].resume && native->entries[i].release && !native->entries[i].environment);
        CHECK(selected->parameter_count == module->functions[i].parameter_count && selected->result == module->functions[i].result);
        for (uint32_t p = 0; p < selected->parameter_count; ++p)
            CHECK(selected->parameters[p] == module->functions[i].parameters[p]);
        f->selected[i] = selected;
        f->base.entries[i] = *selected;
        f->base.entries[i].resume = cm_resume;
        f->base.entries[i].release = cm_release;
        if (f->providers[i] == CM_NATIVE) {
            f->base.bindings[i] = (CnBinding){i, selected};
            f->base.entries[i].environment = &f->base.bindings[i];
        } else CHECK(f->base.entries[i].environment == &f->vm_bindings[i]);
        CHECK(f->base.entries[i].state_bytes == selected->state_bytes);
        CHECK(f->base.entries[i].flags == selected->flags && f->base.entries[i].cleanup_owner == selected->cleanup_owner);
    }
    const XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, native->target, f->base.entries, CN_FUNCTIONS,
        module->declarations, {f, cm_code_drop}, module->types, proof};
    CHECK(xr_xir_compile_program_seal(f->base.context, &spec, &f->base.program) == XR_XIR_OK);
    const XrXirProgramPermissions *permissions = f->base.program->permissions;
    CHECK(permissions && permissions->function_count == CN_FUNCTIONS);
    CHECK(!permissions->entries[CN_PURE].requires_root && !permissions->entries[CN_PURE].unresolved);
    CHECK(!permissions->entries[CN_RELAY].requires_root && !permissions->entries[CN_RELAY].unresolved);
    CHECK(permissions->entries[CN_RELAY].worker == XR_XIR_BAD_TYPE && permissions->entries[CN_CHILD].worker == XR_XIR_OK);
    CHECK(permissions->entries[CN_ROOT_LEAF].requires_root && !permissions->entries[CN_ROOT_LEAF].unresolved);
    CHECK(!permissions->entries[CN_UNKNOWN].requires_root && permissions->entries[CN_UNKNOWN].unresolved);
    CHECK(permissions->entries[CN_MIXED].requires_root && permissions->entries[CN_MIXED].unresolved);
}

static void cm_run_pair(CmFixture *f) {
    const unsigned provider = f->root_native ? CM_NATIVE : CM_VM;
    const uint32_t roles[4] = {CN_ROOT, CN_CHILD, CN_RELAY, CN_PURE};
    /* Each backend advances one opcode per step and consumes invoke inboxes before its next opcode. */
    const unsigned steps[4] = {3u, 5u, 2u, 2u};
    for (unsigned i = 0; i < CN_INSTANCES; ++i) {
        CnWitness *w = &f->base.witnesses[i];
        CnWitness *peer = &f->base.witnesses[1 - i];
        CmWitness *m = &f->witnesses[i];
        CmWitness peer_before = f->witnesses[1 - i];
        const unsigned before = peer->child_callbacks;
        CHECK(xr_xir_instance_start(w->instance, CN_ROOT, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = cn_returned(w);
        CHECK(result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.reserved && result.outcome.value.payload == 42);
        XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(0x12345678)};
        const XrXirValue original = occupied;
        const size_t attempts = runtime_attempts;
        const uint64_t work = xr_xir_domain_budget_stats(w->domain).work;
        CHECK(xr_xir_instance_take_result(w->instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied, &original, sizeof(occupied)) && runtime_attempts == attempts);
        CHECK(xr_xir_domain_budget_stats(w->domain).work == work);
        CHECK(xr_xir_instance_take_result(w->instance, NULL) == XR_XIR_CALL_BAD_ARGUMENT);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(w->instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && !value.reserved && value.payload == 42);
        xr_xir_value_drop(&value);
        CHECK(!value.type && !value.reserved && !value.payload);
        CHECK(xr_xir_instance_take_result(w->instance, &value) == XR_XIR_CALL_BAD_STATE);
        CHECK(!value.type && !value.reserved && !value.payload);
        CHECK(w->probes == 1 && w->child_callbacks && w->typed_outputs == 1 && w->byte_outputs == 1);
        CHECK(w->releases[CN_CHILD] == 1 && w->releases[CN_RELAY] == 1 && w->releases[CN_PURE] == 1);
        CHECK(w->root_call == w->instance->call && peer->child_callbacks == before);
        CHECK(m->child_call && m->child_call != w->root_call);
        CHECK(!memcmp(&peer_before, &f->witnesses[1 - i], sizeof(peer_before)));
        CHECK(m->awaits[provider] == 1 && !m->awaits[1u - provider] && w->await_actions == 1);
        CHECK(m->child_relay[1u - provider] == 1 && !m->child_relay[provider]);
        CHECK(m->relay_pure[provider] == 1 && !m->relay_pure[1u - provider]);
        for (unsigned role = 0; role < 4; ++role) {
            const uint32_t function = roles[role];
            const unsigned selected = f->providers[function];
            if (m->steps[selected][function] != steps[role]) fprintf(stderr,
                "mixed actual steps direction=%s instance=%u function=%u provider=%u actual=%u expected=%u\n",
                f->root_native ? "native-vm" : "vm-native", i, function, selected, m->steps[selected][function], steps[role]);
            CHECK(m->steps[selected][function] == steps[role] && !m->steps[1u - selected][function]);
            CHECK(w->calls[function] == steps[role] && m->releases[selected][function] == 1);
            CHECK(!m->releases[1u - selected][function] && w->releases[function] == 1);
        }
        cn_resolve_expected(&w->saved_root, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0, true, NULL);
    }
}
#endif // XIR_CHILD_CALLABLE_MIXED_ORACLES_H
