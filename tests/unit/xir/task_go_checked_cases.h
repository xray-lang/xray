/* Fixed direct GO/await roles; executable Task lowering is still refused. */
typedef struct TaskGoFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types;
    XrXirType worker_parameter, task_parameter, generic_argument;
    XrXirInstruction worker[4], root[14], maker[2], helper[2], unknown[5], initializer;
    XrXirBlock worker_block, worker_blocks[3], root_blocks[6], maker_block, helper_block, unknown_blocks[3], init_block;
    uint32_t root_operand, maker_operand, helper_operand, worker_operand;
    XrXirFunction functions[7]; XrXirFunctionIdentity identities[7];
    XrXirSourceModule source; XrXirDeclarations declarations; XrXirSlot slot;
    XrXirConstraint bound; XrXirGeneric generics[7]; XrXirModule module;
} TaskGoFixture;
static void task_go_fixture(TaskGoFixture *f) {
    *f = (TaskGoFixture){0};
    f->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    f->nodes[1] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING};
    f->types = (XrXirTypes){f->nodes, 2, NULL, NULL};
    f->worker_parameter = XR_XIR_I64; f->task_parameter = (XrXirType)256;
    f->worker[0] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    f->worker[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->worker_block = (XrXirBlock){.count = 2};
    f->root[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
    f->root[1] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256, .args = {0, 1}};
    f->root[2] = (XrXirInstruction){.op = XR_XIR_COPY, .type = (XrXirType)256, .args = {1}};
    f->root[3] = (XrXirInstruction){.op = XR_XIR_LOCAL_NEW, .type = (XrXirType)256, .args = {2}};
    f->root[4] = (XrXirInstruction){.op = XR_XIR_LOCAL_READ, .type = (XrXirType)256, .args = {3}};
    f->root[5] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {4}, .targets = {1, 2}};
    f->root[6] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 5};
    f->root[7] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {6}};
    f->root[8] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 5};
    f->root[9] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {8}};
    f->root_blocks[0] = (XrXirBlock){.count = 6};
    f->root_blocks[1] = (XrXirBlock){.first = 6, .count = 2};
    f->root_blocks[2] = (XrXirBlock){.first = 8, .count = 2};
    f->maker[0] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256, .args = {0, 1}};
    f->maker[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1}};
    f->helper[0] = (XrXirInstruction){.op = XR_XIR_CALL, .type = (XrXirType)256, .args = {0, 1}, .immediate = 2};
    f->helper[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->unknown[0] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .targets = {1, 2}};
    f->unknown[1] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64};
    f->unknown[2] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {2}};
    f->unknown[3] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR};
    f->unknown[4] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {4}};
    f->unknown_blocks[0] = (XrXirBlock){.count = 1};
    f->unknown_blocks[1] = (XrXirBlock){.first = 1, .count = 2};
    f->unknown_blocks[2] = (XrXirBlock){.first = 3, .count = 2};
    f->initializer = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->maker_block = f->helper_block = (XrXirBlock){.count = 2}; f->init_block = (XrXirBlock){.count = 1};
    f->functions[0] = (XrXirFunction){.name = "worker", .name_length = 6, .parameters = &f->worker_parameter,
        .parameter_count = 1, .result = XR_XIR_I64, .blocks = &f->worker_block, .block_count = 1,
        .instructions = f->worker, .instruction_count = 2};
    f->functions[1] = (XrXirFunction){.name = "root", .name_length = 4, .result = XR_XIR_I64,
        .blocks = f->root_blocks, .block_count = 3, .instructions = f->root, .instruction_count = 10,
        .operands = &f->root_operand, .operand_count = 1};
    f->functions[2] = (XrXirFunction){.name = "maker", .name_length = 5, .parameters = &f->worker_parameter,
        .parameter_count = 1, .result = (XrXirType)256, .blocks = &f->maker_block, .block_count = 1,
        .instructions = f->maker, .instruction_count = 2, .operands = &f->maker_operand, .operand_count = 1};
    f->functions[3] = (XrXirFunction){.name = "helper", .name_length = 6, .parameters = &f->worker_parameter,
        .parameter_count = 1, .blocks = &f->helper_block, .block_count = 1,
        .instructions = f->helper, .instruction_count = 2, .operands = &f->helper_operand, .operand_count = 1};
    f->functions[4] = (XrXirFunction){.name = "unknown", .name_length = 7, .parameters = &f->task_parameter,
        .parameter_count = 1, .result = XR_XIR_I64, .blocks = f->unknown_blocks, .block_count = 3,
        .instructions = f->unknown, .instruction_count = 5};
    f->functions[5] = (XrXirFunction){.name = "init", .name_length = 4, .blocks = &f->init_block,
        .block_count = 1, .instructions = &f->initializer, .instruction_count = 1};
    f->source = (XrXirSourceModule){"m", 1, NULL, 0, 5};
    f->declarations = (XrXirDeclarations){.modules = &f->source, .module_count = 1,
        .functions = f->identities, .root_module = 0, .entry_function = 1};
    f->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = f->functions, .function_count = 6,
        .declarations = &f->declarations, .types = &f->types};
}
static XrXirStatus task_go_pipeline(const XrXirCompileContext *c) {
    TaskGoFixture f; task_go_fixture(&f);
    XrXirArtifact *checked = NULL, *decoded = NULL, *specialized = NULL; XrXirEffects *effects = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(c, &f.module, &checked, &diagnostic);
    if (status != XR_XIR_OK && fail_at == SIZE_MAX)
        fprintf(stderr, "GO check status=%u fn=%u op=%u\n", status, diagnostic.function, diagnostic.instruction);
    if (status == XR_XIR_OK) status = xr_xir_compile_effects_analyze(checked, &effects);
    if (status == XR_XIR_OK) {
        CHECK(xr_xir_effects_go_safe(effects, 0) == XR_XIR_OK);
        CHECK(xr_xir_effects_go_safe(effects, 5) == XR_XIR_OK);
        CHECK(xr_xir_effects_function(effects, 2)->suspend == XR_XIR_EFFECT_NONE &&
            xr_xir_effects_function(effects, 2)->throws == XR_XIR_EFFECT_NONE);
        CHECK(xr_xir_effects_function(effects, 1)->suspend == XR_XIR_EFFECT_MAY &&
            xr_xir_effects_function(effects, 1)->throws == XR_XIR_EFFECT_NONE);
        CHECK(xr_xir_effects_function(effects, 4)->throws == XR_XIR_EFFECT_MAY &&
            xr_xir_effects_error_unidentified(effects, 4));
        CHECK(xr_xir_effects_task_creation(effects, 2) == XR_XIR_EFFECT_MAY &&
            xr_xir_effects_task_creation(effects, 3) == XR_XIR_EFFECT_MAY &&
            xr_xir_effects_task_creation(effects, 4) == XR_XIR_EFFECT_NONE);
        status = xr_xir_compile_checked_write(checked, &packet, NULL);
        if (status == XR_XIR_OK) {
            CHECK(packet.length == sizeof(task_go68_golden));
            for (size_t i = 64; i < packet.length; ++i) if (packet.bytes[i] != task_go68_golden[i]) {
                fprintf(stderr, "GO named packet mismatch offset%zu actual%u expected%u\n",
                    i, packet.bytes[i], task_go68_golden[i]); break;
            }
            CHECK(!memcmp(packet.bytes, task_go68_golden, packet.length));
        }
    }
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_read(c, task_go68_golden, sizeof(task_go68_golden), &decoded, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_specialize(decoded, &specialized, NULL);
    if (status == XR_XIR_OK) {
        memset(f.worker, 0xcc, sizeof(f.worker)); memset(f.root, 0xcc, sizeof(f.root));
        status = xr_xir_compile_artifact_verify(specialized, NULL);
    }
    xr_xir_compile_effects_free(effects); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(checked); return status;
}
static void task_go_reject(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    _Static_assert(XR_XIR_GO == 146 && XR_XIR_TASK_AWAIT == 147 && XR_XIR_OP_COUNT == 149,
        "Task operation ordinals append after the unchanged SlotGroup ordinal");
    for (uint32_t mutation = 0; mutation < 14; ++mutation) {
        TaskGoFixture f; task_go_fixture(&f);
        switch (mutation) {
        case 0: f.root[1].targets[0] = 1; break;
        case 1: f.root[1].type = XR_XIR_I64; break;
        case 2: f.root[1].type = (XrXirType)257; break;
        case 3: f.root[1].immediate = 6; break;
        case 4: f.root[1].args[1] = 0; break;
        case 5: f.root_operand = 100; break;
        case 6: f.root[5].type = XR_XIR_I64; break;
        case 7: f.root[5].args[1] = 1; break;
        case 8: f.root[5].immediate = 1; break;
        case 9: f.root[5].type_arguments[1] = 1; break;
        case 10: f.root[5].targets[1] = 1; break;
        case 11: f.root[6].type = XR_XIR_STRING; break;
        case 12: f.root[8].immediate = 1; break;
        default: f.root[5].args[0] = 3; break;
        }
        XrXirArtifact *out = NULL; XrXirStatus status = xr_xir_compile_check(&c, &f.module, &out, NULL);
        CHECK((status == XR_XIR_BAD_STRUCTURE || status == XR_XIR_BAD_TYPE || status == XR_XIR_BAD_VALUE) && !out);
        CHECK(stats(&c).live_bytes == baseline);
    }
    owner_free(&c, baseline);
}
static void task_go_slots(void) {
    for (uint32_t transitive = 0; transitive < 2; ++transitive)
        for (uint32_t mutable = 0; mutable < 2; ++mutable) {
            XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
            TaskGoFixture f; task_go_fixture(&f);
            f.slot = (XrXirSlot){0, XR_XIR_I64, mutable};
            f.declarations.slots = &f.slot; f.declarations.slot_count = 1;
            if (!transitive) {
                f.worker[0] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64};
                f.worker[1].args[0] = 1;
            } else {
                f.worker[0] = (XrXirInstruction){.op = XR_XIR_CALL, .args = {0, 1}, .immediate = 3};
                f.functions[0].operands = &f.worker_operand; f.functions[0].operand_count = 1;
                f.helper[0] = (XrXirInstruction){.op = XR_XIR_SLOT_LOAD, .type = XR_XIR_I64};
                f.functions[3].operands = NULL; f.functions[3].operand_count = 0;
            }
            XrXirArtifact *out = NULL;
            XrXirDiagnostic d = {0}; XrXirStatus status = xr_xir_compile_check(&c, &f.module, &out, &d);
            if (status != (mutable ? XR_XIR_BAD_TYPE : XR_XIR_OK))
                fprintf(stderr, "GO slot transitive%u mutable%u status%u fn%u op%u\n",
                    transitive, mutable, status, d.function, d.instruction);
            CHECK(status == (mutable ? XR_XIR_BAD_TYPE : XR_XIR_OK));
            xr_xir_compile_artifact_free(out); owner_free(&c, baseline);
        }
}
static void task_go_definition(void) {
    for (uint32_t missing = 0; missing < 2; ++missing) {
        XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
        TaskGoFixture f; task_go_fixture(&f);
        f.bound.markers = missing ? 0 : XR_XIR_CONSTRAINT_SENDABLE;
        f.worker_parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
        f.functions[0].result = f.worker_parameter;
        /* Maker/helper parameters stay concrete, independently of the worker binder. */
        XrXirType concrete = XR_XIR_I64;
        f.functions[2].parameters = f.functions[3].parameters = &concrete;
        f.generic_argument = XR_XIR_I64;
        f.generics[0] = (XrXirGeneric){.constraints = &f.bound, .parameter_count = 1};
        f.generics[1] = f.generics[2] = (XrXirGeneric){.arguments = &f.generic_argument, .argument_count = 1};
        f.module.generics = f.generics; f.root[1].type_arguments[1] = f.maker[0].type_arguments[1] = 1;
        XrXirArtifact *checked = NULL, *specialized = NULL;
        CHECK(xr_xir_compile_check(&c, &f.module, &checked, NULL) == (missing ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        if (!missing) {
            XrXirDiagnostic d = {0};
            XrXirStatus status = xr_xir_compile_specialize(checked, &specialized, &d);
            if (status != XR_XIR_OK) fprintf(stderr, "GO specialize=%u fn=%u op=%u\n", status, d.function, d.instruction);
            CHECK(status == XR_XIR_OK && xr_xir_compile_artifact_verify(specialized, NULL) == XR_XIR_OK);
            const XrXirModule *closed = xr_xir_compile_artifact_module(specialized);
            for (uint32_t fn = 0; fn < closed->function_count; ++fn)
                for (uint32_t i = 0; i < closed->functions[fn].instruction_count; ++i)
                    if (closed->functions[fn].instructions[i].op == XR_XIR_GO)
                        CHECK(!closed->functions[fn].instructions[i].type_arguments[0] &&
                            !closed->functions[fn].instructions[i].type_arguments[1]);
        }
        xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
    }
}
static void task_go_nominals(TaskGoFixture *f, TaskErrorTypes *auth) {
    task_error_types(auth); auth->types.count = 6;
    auth->nodes[4] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    auth->nodes[5] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING};
    f->module.types = &auth->types; f->task_parameter = (XrXirType)260;
    f->functions[2].result = (XrXirType)260;
    for (uint32_t i = 1; i < 5; ++i) f->root[i].type = (XrXirType)260;
    f->maker[0].type = f->helper[0].type = (XrXirType)260;
}
static void task_go_expected(const XrXirCompileContext *c, TaskGoFixture *f,
    XrXirStatus expected, XrXirArtifact **out) {
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(c, &f->module, out, &diagnostic);
    if (status != expected) fprintf(stderr, "GO role expected%u actual%u fn%u op%u\n",
        expected, status, diagnostic.function, diagnostic.instruction);
    CHECK(status == expected && (expected == XR_XIR_OK || !*out));
}
static void task_go_outcomes(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
        TaskGoFixture f; TaskErrorTypes auth; task_go_fixture(&f); task_go_nominals(&f, &auth);
        if (!mode) {
            f.worker[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
            f.worker[1] = (XrXirInstruction){.op = XR_XIR_ENUM_NEW, .type = (XrXirType)257, .args = {0, 1}};
            f.worker[2] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {2}};
            f.worker_operand = 1; f.functions[0].operands = &f.worker_operand; f.functions[0].operand_count = 1;
            f.worker_block.count = f.functions[0].instruction_count = 3;
        } else if (mode == 1) {
            /* Throwing the empty variant cannot grant the complete unsafe enum Sendable. */
            f.worker[0] = (XrXirInstruction){.op = XR_XIR_ENUM_NEW, .type = (XrXirType)259};
            f.worker[1] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {1}};
        } else {
            /* A local class that never crosses an outcome or capture remains legal. */
            f.worker[0] = (XrXirInstruction){.op = XR_XIR_CLASS_NEW, .type = (XrXirType)258};
            f.worker[1] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
            f.worker[2] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {2}};
            f.worker_block.count = f.functions[0].instruction_count = 3;
        }
        XrXirArtifact *checked = NULL; task_go_expected(&c, &f, mode == 1 ? XR_XIR_BAD_TYPE : XR_XIR_OK, &checked);
        if (checked) {
            XrXirEffects *effects = NULL;
            CHECK(xr_xir_compile_effects_analyze(checked, &effects) == XR_XIR_OK);
            CHECK(xr_xir_effects_go_safe(effects, 0) == XR_XIR_OK);
            CHECK(xr_xir_effects_function(effects, 2)->throws == XR_XIR_EFFECT_NONE &&
                xr_xir_effects_function(effects, 2)->suspend == XR_XIR_EFFECT_NONE);
            CHECK(xr_xir_effects_error(effects, 1, (XrXirType)257, 0) == !mode &&
                !xr_xir_effects_error(effects, 1, (XrXirType)257, 1) &&
                !xr_xir_effects_error_unidentified(effects, 1));
            xr_xir_compile_effects_free(effects);
        }
        xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
    }
}
static void task_go_caught(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    TaskGoFixture f; TaskErrorTypes auth; task_go_fixture(&f); task_go_nominals(&f, &auth);
    f.helper[0] = (XrXirInstruction){.op = XR_XIR_ENUM_NEW, .type = (XrXirType)259};
    f.helper[1] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {1}};
    f.functions[3].operands = NULL; f.functions[3].operand_count = 0;
    f.worker[0] = (XrXirInstruction){.op = XR_XIR_INVOKE, .args = {0, 1}, .targets = {1, 2}, .immediate = 3};
    f.worker[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f.worker[2] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR};
    f.worker[3] = (XrXirInstruction){.op = XR_XIR_RETURN};
    XrXirBlock blocks[3] = {{.count = 1}, {.first = 1, .count = 1}, {.first = 2, .count = 2}};
    f.functions[0].blocks = blocks; f.functions[0].block_count = 3; f.functions[0].instruction_count = 4;
    f.functions[0].operands = &f.worker_operand; f.functions[0].operand_count = 1;
    XrXirArtifact *checked = NULL; task_go_expected(&c, &f, XR_XIR_OK, &checked);
    XrXirEffects *effects = NULL; CHECK(xr_xir_compile_effects_analyze(checked, &effects) == XR_XIR_OK);
    CHECK(xr_xir_effects_task_errors(effects, 3) == XR_XIR_BAD_TYPE &&
        xr_xir_effects_task_errors(effects, 0) == XR_XIR_OK &&
        xr_xir_effects_function(effects, 0)->throws == XR_XIR_EFFECT_NONE);
    xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
}
static void task_go_cleanup(void) {
    for (uint32_t direct = 0; direct < 2; ++direct) for (uint32_t cleanup = 0; cleanup < 2; ++cleanup) {
        XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
        TaskGoFixture f; task_go_fixture(&f); f.identities[3].cleanup_owner = cleanup;
        if (direct) f.helper[0] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256, .args = {0, 1}};
        XrXirArtifact *checked = NULL;
        task_go_expected(&c, &f, cleanup ? XR_XIR_BAD_TYPE : XR_XIR_OK, &checked);
        xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
    }
}
static void task_go_visibility(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
        TaskGoFixture f; task_go_fixture(&f); uint32_t dependency = 1;
        XrXirSourceModule modules[2] = {{"m", 1, &dependency, mode ? 1 : 0, 5}, {"n", 1, NULL, 0, 6}};
        f.identities[0].module = f.identities[6].module = 1;
        f.identities[0].exported = mode != 1;
        f.functions[6] = (XrXirFunction){.name = "initN", .name_length = 5, .blocks = &f.init_block,
            .block_count = 1, .instructions = &f.initializer, .instruction_count = 1};
        f.module.function_count = 7; f.declarations.modules = modules; f.declarations.module_count = 2;
        XrXirArtifact *checked = NULL; task_go_expected(&c, &f, mode == 2 ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE, &checked);
        xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
    }
}
static void task_go_phi(void) {
    for (uint32_t unknown = 0; unknown < 2; ++unknown) {
        XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
        TaskGoFixture f; TaskErrorTypes auth; task_go_fixture(&f); task_go_nominals(&f, &auth);
        f.worker[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
        f.worker[1] = (XrXirInstruction){.op = XR_XIR_ENUM_NEW, .type = (XrXirType)257, .args = {0, 1}};
        f.worker[2] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {2}};
        f.worker_operand = 1; f.functions[0].operands = &f.worker_operand; f.functions[0].operand_count = 1;
        f.worker_block.count = f.functions[0].instruction_count = 3;
        f.root[2] = (XrXirInstruction){.op = XR_XIR_CONST_BOOL, .type = XR_XIR_BOOL, .immediate = 1};
        f.root[3] = (XrXirInstruction){.op = XR_XIR_BRANCH, .args = {2}, .targets = {1, 2}};
        f.root[4] = (XrXirInstruction){.op = XR_XIR_COPY, .type = (XrXirType)260, .args = {1}};
        f.root[5] = (XrXirInstruction){.op = XR_XIR_JUMP, .targets = {3}};
        f.root[6] = unknown ? (XrXirInstruction){.op = XR_XIR_CALL, .type = (XrXirType)260,
            .args = {1, 1}, .immediate = 2} : f.root[4];
        f.root[7] = f.root[5];
        f.root[8] = (XrXirInstruction){.op = XR_XIR_PHI, .type = (XrXirType)260, .args = {1 + unknown, 4}};
        f.root[9] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {8}, .targets = {4, 5}};
        f.root[10] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 9};
        f.root[11] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {10}};
        f.root[12] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 9};
        f.root[13] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {12}};
        f.root_blocks[0] = (XrXirBlock){.count = 4};
        for (uint32_t b = 1; b < 6; ++b) f.root_blocks[b] = (XrXirBlock){.first = 2 + 2 * b, .count = 2};
        uint32_t operands[6] = {0, 1, 4, 2, 6, 0};
        if (unknown) { operands[1] = 0; operands[2] = 1; operands[3] = 4; operands[4] = 2; operands[5] = 6; }
        f.functions[1].instruction_count = 14; f.functions[1].block_count = 6;
        f.functions[1].operands = operands; f.functions[1].operand_count = 5 + unknown;
        XrXirArtifact *checked = NULL; task_go_expected(&c, &f, XR_XIR_OK, &checked);
        XrXirEffects *effects = NULL; CHECK(xr_xir_compile_effects_analyze(checked, &effects) == XR_XIR_OK);
        CHECK(xr_xir_effects_error(effects, 1, (XrXirType)257, 0) &&
            !xr_xir_effects_error(effects, 1, (XrXirType)257, 1) &&
            xr_xir_effects_error_unidentified(effects, 1) == (unknown != 0));
        xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
    }
}
static void task_go_axes(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    CHECK(task_go_pipeline(&c) == XR_XIR_OK); XrCompileResourceStats cost = stats(&c); owner_free(&c, baseline);
    const uint64_t exact[] = {cost.allocated_bytes, cost.peak_bytes, cost.work};
    for (uint32_t axis = 0; axis < 3; ++axis) for (uint32_t minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = caps(); CHECK(exact[axis]);
        if (!axis) limits.allocated_bytes = exact[axis] - minus;
        else if (axis == 1) limits.live_bytes = exact[axis] - minus;
        else limits.work = exact[axis] - minus;
        c = owner_new(limits); baseline = stats(&c).live_bytes;
        CHECK(task_go_pipeline(&c) == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(stats(&c).allocated_bytes <= limits.allocated_bytes &&
            stats(&c).peak_bytes <= limits.live_bytes && stats(&c).work <= limits.work);
        owner_free(&c, baseline);
    }
    printf("Task GO/AWAIT same-ledger axes allocated/live/work=%llu/%llu/%llu exact/minus1 physical0\n",
        (unsigned long long)cost.allocated_bytes, (unsigned long long)cost.peak_bytes, (unsigned long long)cost.work);
}
static void task_go_packets(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    const uint8_t *bad[] = {task_go68_scalar_go, task_go68_initializer_target,
        task_go68_await_place, task_go68_go_target, task_go68_await_binding,
        task_go68_mutable_worker, task_go68_cleanup_helper};
    const size_t lengths[] = {sizeof(task_go68_scalar_go), sizeof(task_go68_initializer_target),
        sizeof(task_go68_await_place), sizeof(task_go68_go_target), sizeof(task_go68_await_binding),
        sizeof(task_go68_mutable_worker), sizeof(task_go68_cleanup_helper)};
    const uint8_t *historical[] = {task_go66_scalar_go, task_go66_initializer_target,
        task_go66_await_place, task_go66_go_target, task_go66_await_binding,
        task_go66_mutable_worker, task_go66_cleanup_helper};
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        XrXirArtifact *out = NULL; size_t before = attempts;
        CHECK(xr_xir_compile_checked_read(&c, historical[i], lengths[i], &out, NULL) ==
            XR_XIR_BAD_STRUCTURE && !out && attempts == before);
        out = (XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(&c, historical[i], lengths[i], &out, NULL) ==
            XR_XIR_BAD_STRUCTURE && out == (XrXirArtifact *)(uintptr_t)1 && attempts == before);
        out = NULL;
        XrXirStatus status = xr_xir_compile_checked_read(&c, bad[i], lengths[i], &out, NULL);
        CHECK((status == XR_XIR_BAD_STRUCTURE || status == XR_XIR_BAD_TYPE || status == XR_XIR_BAD_VALUE) && !out);
        CHECK(attempts > before && bad[i][8] == 25 && bad[i][12] == 68);
        out = (XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(&c, bad[i], lengths[i], &out, NULL) == status &&
            out == (XrXirArtifact *)(uintptr_t)1 && stats(&c).live_bytes == baseline);
    }
    owner_free(&c, baseline);
}
static void task_go_previous67_packets(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    const uint8_t *packets[] = {task_types67_golden, task_unit67_golden, task_go67_golden,
        task_go67_scalar_go, task_go67_initializer_target, task_go67_await_place,
        task_go67_go_target, task_go67_await_binding, task_go67_mutable_worker, task_go67_cleanup_helper};
    const size_t lengths[] = {sizeof(task_types67_golden), sizeof(task_unit67_golden), sizeof(task_go67_golden),
        sizeof(task_go67_scalar_go), sizeof(task_go67_initializer_target), sizeof(task_go67_await_place),
        sizeof(task_go67_go_target), sizeof(task_go67_await_binding), sizeof(task_go67_mutable_worker), sizeof(task_go67_cleanup_helper)};
    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        XrXirArtifact *out = NULL; size_t before = attempts;
        CHECK(xr_xir_compile_checked_read(&c, packets[i], lengths[i], &out, NULL) ==
            XR_XIR_BAD_STRUCTURE && !out && attempts == before);
        out = (XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(&c, packets[i], lengths[i], &out, NULL) ==
            XR_XIR_BAD_STRUCTURE && out == (XrXirArtifact *)(uintptr_t)1 && attempts == before);
        CHECK(stats(&c).live_bytes == baseline);
    }
    owner_free(&c, baseline);
}
static void task_go_cleanup_fixture(TaskGoFixture *f, uint32_t spawn, uint32_t mutable) {
        task_go_fixture(f);
        f->identities[3].cleanup_owner = 1;
        f->slot = (XrXirSlot){0, XR_XIR_I64, 1}; f->declarations.slots = &f->slot; f->declarations.slot_count = 1;
        f->worker[0] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .args = {0, 1}, .targets = {1}, .immediate = 3};
        f->worker[1] = (XrXirInstruction){.op = XR_XIR_CLEANUP_LEAVE, .targets = {2}};
        f->worker[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
        f->worker_blocks[0] = (XrXirBlock){.count = 1};
        f->worker_blocks[1] = (XrXirBlock){.first = 1, .count = 1, .frontier = 1};
        f->worker_blocks[2] = (XrXirBlock){.first = 2, .count = 1};
        f->functions[0].blocks = f->worker_blocks; f->functions[0].block_count = 3; f->functions[0].instruction_count = 3;
        f->functions[0].operands = &f->worker_operand; f->functions[0].operand_count = 1;
        f->helper[0] = mutable ? (XrXirInstruction){.op = XR_XIR_SLOT_STORE} :
            (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 9};
        f->helper[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
        f->functions[3].operands = NULL; f->functions[3].operand_count = 0;
        if (!spawn) {
            f->root[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
            f->functions[1].instruction_count = 2; f->functions[1].block_count = 1; f->root_blocks[0].count = 2;
            f->functions[1].operands = NULL; f->functions[1].operand_count = 0;
            f->maker[0] = (XrXirInstruction){.op = XR_XIR_CALL, .type = XR_XIR_I64, .args = {0, 1}};
            f->functions[2].result = XR_XIR_I64;
        }
}
static void task_go_cleanup_permissions(void) {
    for (uint32_t spawn = 0; spawn < 2; ++spawn) for (uint32_t mutable = 0; mutable < 2; ++mutable) {
        XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
        TaskGoFixture f; task_go_cleanup_fixture(&f, spawn, mutable);
        XrXirArtifact *checked = NULL;
        task_go_expected(&c, &f, spawn && mutable ? XR_XIR_BAD_TYPE : XR_XIR_OK, &checked);
        xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
    }
}
static XrXirStatus task_go_cleanup_pipeline(const XrXirCompileContext *c) {
    TaskGoFixture f; task_go_cleanup_fixture(&f, 1, 0);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL; XrXirEffects *effects = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_check(c, &f.module, &checked, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_effects_analyze(checked, &effects);
    if (status == XR_XIR_OK) {
        CHECK(xr_xir_effects_go_safe(effects, 0) == XR_XIR_OK && xr_xir_effects_go_safe(effects, 3) == XR_XIR_OK);
        status = xr_xir_compile_checked_write(checked, &packet, NULL);
    }
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_read(c, packet.bytes, packet.length, &decoded, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_specialize(decoded, &closed, NULL);
    if (status == XR_XIR_OK) {
        memset(f.worker, 0xcc, sizeof(f.worker)); memset(f.helper, 0xcc, sizeof(f.helper));
        status = xr_xir_compile_artifact_verify(closed, NULL);
    }
    xr_xir_compile_effects_free(effects); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(checked); return status;
}
