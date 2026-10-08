/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_stateful.inc.c - Original mutable module state
 *
 * KEY CONCEPT:
 *   Repeated calls preserve the original state and escaped assertion owner.
 */
static bool consumer_stateful_case(void) {
    return !strcmp(XR_CONSUMER_NAME, "narrow_array");
}

static unsigned consumer_repeat_count(void) {
    return consumer_stateful_case() ? 1 : 2;
}

static void consumer_stateful_finish(XrXirInstance *instance, uint32_t answer, unsigned ordinal) {
    CHECK(consumer_stateful_case());
    CHECK(xr_xir_instance_start(instance, answer, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult failed = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    CHECK(failed.outcome.status == XR_XIR_CALL_ASSERTION);
    CHECK(failed.outcome.panic.detail.code == XR_XIR_PANIC_ASSERTION);
    XrXirCallResult owned = {0};
    CHECK(xr_xir_call_result_copy(&failed.outcome, &owned) == XR_XIR_VALUE_OK);
    const char *before_bytes = NULL;
    size_t before_length = 0;
    CHECK(xr_xir_panic_valid(&owned.panic));
    CHECK(xr_xir_string_view(&owned.panic.message, &before_bytes, &before_length));
    char golden[256];
    CHECK(before_length < sizeof(golden));
    if (before_length)
        memcpy(golden, before_bytes, before_length);
    XrXirValue untouched = {0};
    CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
    CHECK(!untouched.type && !untouched.payload);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    const char *after_bytes = NULL;
    size_t after_length = 0;
    CHECK(xr_xir_panic_valid(&owned.panic));
    CHECK(xr_xir_string_view(&owned.panic.message, &after_bytes, &after_length));
    CHECK(after_length == before_length);
    if (after_length)
        CHECK(!memcmp(golden, after_bytes, after_length));
    printf("stateful-repeat instance=%u status=%u panic=%u owned-message=%zu\n",
        ordinal, owned.status, owned.panic.detail.code, after_length);
    xr_xir_call_result_drop(&owned);
}

static void consumer_stateful_instruction(const XrXirInstruction *actual, XrXirInstruction expected) {
    CHECK(actual->op == expected.op && actual->type == expected.type && actual->immediate == expected.immediate);
    CHECK(actual->args[0] == expected.args[0] && actual->args[1] == expected.args[1]);
    CHECK(!actual->targets[0] && !actual->targets[1] && !actual->type_arguments[0] && !actual->type_arguments[1]);
}

static void consumer_stateful_linear(const XrXirFunction *fn, uint32_t count, XrXirType result) {
    CHECK(!fn->parameter_count && fn->result == result && fn->block_count == 1);
    CHECK(fn->instruction_count == count && !fn->operand_count);
    CHECK(fn->blocks[0].first == 0 && fn->blocks[0].count == count);
    CHECK(!fn->blocks[0].panic && !fn->blocks[0].frontier);
}

static void consumer_stateful_shape(const XrXirModule *module, const Consumer *run) {
    CHECK(!strcmp(XR_CONSUMER_NAME, "narrow_array"));
    const XrXirDeclarations *d = module->declarations;
    CHECK(d && d->module_count == 2 && d->root_module == 0 && d->slot_count == 2);
    CHECK(module->function_count == 9 && run->state_probe < module->function_count);
    uint32_t next = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        CHECK(!d->functions[f].cleanup_owner);
        if (fn->name_length == 4 && !memcmp(fn->name, "next", 4)) {
            CHECK(next == UINT32_MAX && !d->functions[f].exported && d->functions[f].module == d->root_module);
            next = f;
        }
        for (uint32_t b = 0; b < fn->block_count; ++b)
            CHECK(!fn->blocks[b].panic && !fn->blocks[b].frontier);
        if (consumer_fixture->native_program) {
            CHECK(product_consumer_program.entry_count == module->function_count);
            CHECK(!product_consumer_program.entries[f].flags && !product_consumer_program.entries[f].cleanup_owner);
        }
    }
    CHECK(next != UINT32_MAX && next != run->answer && next != run->private_answer && next != run->state_probe);
    const uint32_t roles[] = {next, run->private_answer, run->answer, run->state_probe};
    for (unsigned i = 0; i < 4; ++i) {
        const XrXirFunctionIdentity *identity = &d->functions[roles[i]];
        CHECK(identity->module == d->root_module && !identity->nominal_owner && !identity->member_access);
        CHECK(!identity->method_kind && !identity->test_role && !identity->test_timeout_seconds);
        CHECK(identity->exported == (i >= 2 ? 1u : 0u));
    }
    const XrXirFunction *child = &module->functions[next];
    consumer_stateful_linear(child, 7, XR_XIR_U8);
    CHECK(child->instructions[0].immediate == 1);
    uint32_t visits = (uint32_t)child->instructions[0].immediate;
    CHECK(d->slots[visits].module == d->root_module && d->slots[visits].type == XR_XIR_I64 && d->slots[visits].mutable == 1);
    CHECK(d->slots[0].module == d->root_module && !d->slots[0].mutable);
    CHECK(xr_xir_type_is_array(module->types, d->slots[0].type));
    CHECK(xr_xir_array_element(module->types, d->slots[0].type) == XR_XIR_U8);
    const XrXirInstruction next_expected[] = {
        {XR_XIR_SLOT_LOAD, XR_XIR_I64, {0, 0}, {0, 0}, visits, {0, 0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 1, {0, 0}},
        {XR_XIR_ADD_INT, XR_XIR_I64, {0, 1}, {0, 0}, 0, {0, 0}},
        {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {2, 0}, {0, 0}, visits, {0, 0}},
        {XR_XIR_SLOT_LOAD, XR_XIR_I64, {0, 0}, {0, 0}, visits, {0, 0}},
        {XR_XIR_CONVERT_NUMBER, XR_XIR_U8, {4, 0}, {0, 0}, 0, {0, 0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5, 0}, {0, 0}, 0, {0, 0}}
    };
    for (uint32_t i = 0; i < 7; ++i) consumer_stateful_instruction(&child->instructions[i], next_expected[i]);
    const XrXirFunction *wrapper = &module->functions[run->answer];
    consumer_stateful_linear(wrapper, 2, XR_XIR_I64);
    consumer_stateful_instruction(&wrapper->instructions[0],
        (XrXirInstruction){XR_XIR_CALL, XR_XIR_I64, {0, 0}, {0, 0}, run->private_answer, {0, 0}});
    consumer_stateful_instruction(&wrapper->instructions[1],
        (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0, 0}});
    const XrXirFunction *getter = &module->functions[run->state_probe];
    consumer_stateful_linear(getter, 2, XR_XIR_I64);
    consumer_stateful_instruction(&getter->instructions[0],
        (XrXirInstruction){XR_XIR_SLOT_LOAD, XR_XIR_I64, {0, 0}, {0, 0}, visits, {0, 0}});
    consumer_stateful_instruction(&getter->instructions[1],
        (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0, 0}});
    const XrXirFunction *answer = &module->functions[run->private_answer];
    CHECK(!answer->parameter_count && answer->result == XR_XIR_I64 && answer->instruction_count >= 4);
    CHECK(answer->block_count && !answer->blocks[0].first && answer->blocks[0].count >= 4);
    for (uint32_t i = 0; i < 3; ++i)
        consumer_stateful_instruction(&answer->instructions[i],
            (XrXirInstruction){XR_XIR_CALL, XR_XIR_U8, {0, 0}, {0, 0}, next, {0, 0}});
    CHECK(answer->operand_count >= 3 && answer->operands[0] == 0 && answer->operands[1] == 1 && answer->operands[2] == 2);
    consumer_stateful_instruction(&answer->instructions[3],
        (XrXirInstruction){XR_XIR_ARRAY_NEW, d->slots[0].type, {0, 3}, {0, 0}, 0, {0, 0}});
    uint32_t calls = 0, stores = 0, inits = 0, read_places = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            CHECK(op->op != XR_XIR_CALL_INDIRECT && op->op != XR_XIR_INVOKE && op->op != XR_XIR_INVOKE_INDIRECT);
            CHECK(op->op != XR_XIR_CALL_DEFAULT && op->op != XR_XIR_INVOKE_DEFAULT && op->op != XR_XIR_CALL_REQUIREMENT);
            CHECK(op->op != XR_XIR_SUSPEND && op->op != XR_XIR_TIMER_AFTER_MS && op->op != XR_XIR_GO && op->op != XR_XIR_TASK_AWAIT);
            CHECK(op->op != XR_XIR_CLEANUP_REGISTER && op->op != XR_XIR_CLEANUP_LEAVE && op->op != XR_XIR_CLEANUP_ERROR);
            CHECK(op->op != XR_XIR_PLACE_WRITE && op->op != XR_XIR_SLOT_GROUP_INIT);
            if (op->op == XR_XIR_SLOT_PLACE) {
                CHECK(f == run->private_answer && (i == 40 || i == 50 || i == 75));
                CHECK(i + 2 < fn->instruction_count);
                uint64_t index = i == 40 ? 0 : i == 50 ? 1 : 2;
                consumer_stateful_instruction(op,
                    (XrXirInstruction){XR_XIR_SLOT_PLACE, d->slots[0].type, {0, 0}, {0, 0}, 0, {0, 0}});
                consumer_stateful_instruction(&fn->instructions[i + 1],
                    (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, index, {0, 0}});
                consumer_stateful_instruction(&fn->instructions[i + 2],
                    (XrXirInstruction){XR_XIR_ARRAY_GET, XR_XIR_U8, {i, i + 1}, {0, 0}, 0, {0, 0}});
                ++read_places;
            }
            if (op->op == XR_XIR_CALL && op->immediate == next) {
                CHECK(f == run->private_answer && i < 3);
                ++calls;
            }
            if (op->op == XR_XIR_SLOT_STORE && op->immediate == visits) {
                CHECK(f == next && i == 3);
                ++stores;
            }
            if (op->op == XR_XIR_SLOT_INIT && op->immediate == visits) {
                CHECK(f == d->modules[d->root_module].initializer && op->args[0] < fn->instruction_count);
                const XrXirInstruction *zero = &fn->instructions[op->args[0]];
                CHECK(zero->op == XR_XIR_CONST_INT && zero->type == XR_XIR_I64 && !zero->immediate);
                ++inits;
            }
        }
    }
    CHECK(calls == 3 && stores == 1 && inits == 1 && read_places == 3);
}

static int64_t consumer_stateful_visits(XrXirInstance *instance, uint32_t probe) {
    XrXirValue value = execute(instance, probe, 0);
    CHECK(value.type == XR_XIR_I64 && !value.reserved);
    int64_t result = (int64_t)value.payload;
    xr_xir_value_drop(&value);
    return result;
}

static void consumer_stateful_no_value(XrXirInstance *instance) {
    XrXirValue untouched;
    unsigned char before[sizeof(untouched)];
    memset(&untouched, 0xa5, sizeof(untouched));
    memcpy(before, &untouched, sizeof(before));
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(before, &untouched, sizeof(before)) && runtime_attempts == attempts);
    memset(&untouched, 0, sizeof(untouched));
    memcpy(before, &untouched, sizeof(before));
    CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(before, &untouched, sizeof(before)) && runtime_attempts == attempts);
}

static void consumer_stateful_message(const XrXirCallResult *owned, const char *golden, size_t length) {
    const char *bytes = NULL;
    size_t actual = 0;
    CHECK(owned->status == XR_XIR_CALL_ASSERTION && owned->panic.detail.code == XR_XIR_PANIC_ASSERTION);
    CHECK(xr_xir_panic_valid(&owned->panic));
    CHECK(xr_xir_string_view(&owned->panic.message, &bytes, &actual) && actual == length);
    CHECK(!length || !memcmp(golden, bytes, length));
}

static void consumer_stateful_cancel_finish(XrXirInstance *instance, const Consumer *run,
                                            size_t prefix, int64_t expected) {
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && expected >= 0 && expected <= 3);
    CHECK(consumer_stateful_visits(instance, run->state_probe) == expected);
    CHECK(consumer_stateful_visits(instance, run->state_probe) == expected);
    if (!expected) {
        XrXirValue value = execute(instance, run->answer, 0);
        CHECK(value.type == XR_XIR_I64 && (int64_t)value.payload == 42);
        xr_xir_value_drop(&value);
        CHECK(consumer_stateful_visits(instance, run->state_probe) == 3);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes && !runtime_owned_capacity);
        printf("narrow-cancel prefix=%zu visits=0 repeat-status=%u final-visits=3 physical=0/0 table=0\n",
            prefix, (unsigned)XR_XIR_CALL_RETURNED);
        return;
    }
    CHECK(xr_xir_instance_start(instance, run->answer, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult failed = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    CHECK(failed.outcome.status == XR_XIR_CALL_ASSERTION);
    CHECK(failed.outcome.panic.detail.code == XR_XIR_PANIC_ASSERTION);
    XrXirCallResult owned = {0};
    CHECK(xr_xir_call_result_copy(&failed.outcome, &owned) == XR_XIR_VALUE_OK);
    const char *bytes = NULL;
    size_t length = 0;
    CHECK(xr_xir_panic_valid(&owned.panic));
    CHECK(xr_xir_string_view(&owned.panic.message, &bytes, &length));
    char golden[256];
    CHECK(length < sizeof(golden));
    if (length) memcpy(golden, bytes, length);
    consumer_stateful_no_value(instance);
    XrXirCallResult absent = {0};
    unsigned char absent_before[sizeof(absent)];
    memcpy(absent_before, &absent, sizeof(absent));
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_copy_failure(instance, &absent) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(absent_before, &absent, sizeof(absent)) && runtime_attempts == attempts);
    CHECK(xr_xir_instance_start(instance, run->state_probe, NULL, 0) == XR_XIR_CALL_READY);
    consumer_stateful_message(&owned, golden, length);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_C(1000000)).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue final = {0};
    CHECK(xr_xir_instance_take_result(instance, &final) == XR_XIR_CALL_RETURNED);
    CHECK(final.type == XR_XIR_I64 && (int64_t)final.payload == expected + 3);
    xr_xir_value_drop(&final);
    consumer_stateful_message(&owned, golden, length);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    consumer_stateful_message(&owned, golden, length);
    printf("narrow-cancel prefix=%zu visits=%lld repeat-status=%u final-visits=%lld owned-message=%zu\n",
        prefix, (long long)expected, (unsigned)owned.status, (long long)(expected + 3), length);
    xr_xir_call_result_drop(&owned);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned_capacity);
    puts("narrow-cancel-owned-panic escaped=1 physical=0/0 table=0");
}
