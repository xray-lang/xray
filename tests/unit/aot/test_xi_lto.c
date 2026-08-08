#include "../../../src/aot/xi_lto.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>

static int passed, failed;

#define ASSERT(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d\n", #c, __LINE__);                                                  \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_init_fail(void) {
    XiLtoContext ctx;
    ASSERT(!xi_lto_context_init(&ctx, NULL, 0));
    passed++;
}

static void test_imported_scalar_const_refreshes_native_representations(void) {
    XrType t_unit = {.kind = XR_KIND_UNIT, .id = 1, .frozen = true};
    XrType t_int = {.kind = XR_KIND_INT, .id = 2, .frozen = true};
    XrType t_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
    XrType t_unknown = {.kind = XR_KIND_UNKNOWN, .id = 4, .frozen = true};
    XrType t_u64 = {
        .kind = XR_KIND_INT,
        .id = 5,
        .frozen = true,
        .scalar_rep = XR_NATIVE_U64,
    };

    XiFunc *lib_init = xi_func_new("<lib>", &t_unit);
    XiFunc *app_init = xi_func_new("<app>", &t_bool);
    ASSERT(lib_init && app_init);
    XiBlock *lib_entry = xi_block_new(lib_init);
    XiBlock *app_entry = xi_block_new(app_init);
    ASSERT(lib_entry && app_entry);

    XiValue *limit = xi_const_int(lib_init, lib_entry, 240, &t_int);
    XiValue *publish = xi_value_new(lib_init, lib_entry, XI_SET_SHARED, &t_unit, 1);
    ASSERT(limit && publish);
    publish->args[0] = limit;
    publish->aux_int = 0;
    xi_block_set_return(lib_entry, NULL);

    XiImportRef limit_ref = {
        .module_path = "./lib",
        .member_name = "LIMIT",
        .resolved_mod_index = 0,
        .resolved_shared_slot = 0,
        .resolved_export_slot = -1,
    };
    XiValue *length = xi_const_int(app_init, app_entry, 16, &t_u64);
    XiValue *imported = xi_value_new(app_init, app_entry, XI_GET_SHARED, &t_unknown, 0);
    XiValue *wide_imported = xi_value_new(app_init, app_entry, XI_AS, &t_u64, 1);
    ASSERT(wide_imported);
    wide_imported->args[0] = imported;
    wide_imported->aux_int = (int64_t) (uint32_t) -1 << 1;
    XiValue *within = xi_binary(app_init, app_entry, XI_LE, &t_bool, length, wide_imported);
    XiValue *republish = xi_value_new(app_init, app_entry, XI_SET_SHARED, &t_unit, 1);
    ASSERT(length && imported && within && republish);
    imported->aux_int = 0;
    republish->args[0] = imported;
    republish->aux_int = 1;
    xi_block_set_return(app_entry, within);

    XiConstLiteral lib_literals[1] = {{
        .kind = XI_CONST_LITERAL_INT,
        /* Exercise the linker's exporter-IR type recovery: serialized import
         * metadata may still carry the tagged placeholder used by GET_SHARED. */
        .type = &t_unknown,
        .int_value = 240,
    }};
    XiImportRef *app_imports[2] = {&limit_ref, NULL};
    XiModule lib = {
        .path = "lib.xr",
        .name = "lib",
        .init = lib_init,
        .slot_const_literals = lib_literals,
        .nslots = 1,
    };
    XiModule app = {
        .path = "app.xr",
        .name = "app",
        .init = app_init,
        .slot_imports = app_imports,
        .nslots = 2,
    };
    XiModule *modules[2] = {&lib, &app};

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    xi_opt_refresh_representations_with_policy(lib_init, &policy);
    xi_opt_refresh_representations_with_policy(app_init, &policy);
    ASSERT(imported->rep == XR_REP_TAGGED);
    ASSERT(wide_imported->rep == XR_REP_TAGGED);
    ASSERT(within->args[0] == length);
    ASSERT(republish->args[0] == imported);

    XiLtoContext ctx;
    ASSERT(xi_lto_context_init(&ctx, modules, 2));
    ASSERT(xi_lto_link_modules(&ctx) == 1);
    ASSERT(imported->op == XI_CONST);
    ASSERT(imported->type == &t_int);
    ASSERT(imported->aux_int == 240);
    ASSERT(imported->rep == XR_REP_I64);
    ASSERT(wide_imported->type == &t_u64);
    ASSERT(wide_imported->rep == XR_REP_I64);
    ASSERT(wide_imported->args[0] == imported);
    ASSERT(within->args[0] == length);
    ASSERT(within->args[1] == wide_imported);
    ASSERT(republish->args[0] != imported && republish->args[0]->op == XI_BOX);
    ASSERT(republish->args[0]->args[0] == imported);

    uint32_t app_value_count = app_entry->nvalues;
    ASSERT(xi_lto_link_modules(&ctx) == 0);
    ASSERT(app_entry->nvalues == app_value_count);
    xi_lto_context_free(&ctx);
    xi_func_free(app_init);
    xi_func_free(lib_init);
    passed++;
}

int main(void) {
    test_init_fail();
    test_imported_scalar_const_refreshes_native_representations();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
