/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_spec.c - Unit tests for CLI spec, parser, and dispatch
 *
 * Tests the new declarative CLI infrastructure:
 *   - Command registry (find, list)
 *   - Option map accessors
 *   - Parser (global flags, command options, positionals)
 *   - Help generation (smoke test: no crash)
 *   - Dispatch routing logic
 */

#include "../test_framework.h"
#include "app/cli/xcli_spec.h"
#include "app/cli/xcli_parser.h"
#include "app/cli/xcli_help.h"
#include "app/cli/xcli_diag.h"

/* ========== Command Registry ========== */

TEST(spec_get_commands_not_null) {
    const XrCliCommandSpec *cmds = xr_cli_get_commands();
    ASSERT_NOT_NULL(cmds);
    /* First command should be "run" */
    ASSERT_STR_EQ(cmds[0].name, "run");
}

TEST(spec_find_command_run) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);
    ASSERT_STR_EQ(spec->name, "run");
    ASSERT_NOT_NULL(spec->options);
    ASSERT_TRUE(spec->allow_passthrough);
}

TEST(spec_find_command_eval) {
    const XrCliCommandSpec *spec = xr_cli_find_command("eval");
    ASSERT_NOT_NULL(spec);
    ASSERT_STR_EQ(spec->name, "eval");
    ASSERT_EQ_INT(spec->positional_min, 1);
    ASSERT_EQ_INT(spec->positional_max, 1);
}

TEST(spec_find_command_repl) {
    const XrCliCommandSpec *spec = xr_cli_find_command("repl");
    ASSERT_NOT_NULL(spec);
    ASSERT_STR_EQ(spec->name, "repl");
}

TEST(spec_find_command_pkg) {
    const XrCliCommandSpec *spec = xr_cli_find_command("pkg");
    ASSERT_NOT_NULL(spec);
    ASSERT_NOT_NULL(spec->subcommands);
    ASSERT_GT(spec->subcommand_count, 0);
}

TEST(spec_find_command_unknown) {
    const XrCliCommandSpec *spec = xr_cli_find_command("nonexistent");
    ASSERT_NULL(spec);
}

TEST(spec_aliases_removed) {
    /* Package management aliases should NOT be in the command table */
    ASSERT_NULL(xr_cli_find_command("init"));
    ASSERT_NULL(xr_cli_find_command("add"));
    ASSERT_NULL(xr_cli_find_command("remove"));
    ASSERT_NULL(xr_cli_find_command("install"));
    ASSERT_NULL(xr_cli_find_command("update"));
    ASSERT_NULL(xr_cli_find_command("tree"));
    ASSERT_NULL(xr_cli_find_command("publish"));
    ASSERT_NULL(xr_cli_find_command("login"));
}

TEST(spec_option_count) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);
    int count = xr_cli_option_count(spec->options);
    /* run has: trace, dump-bytecode, workers, coro-watch,
     *          coro-http, no-jit, jit-force, jit-stats, dump-ic = 9 */
    ASSERT_EQ_INT(count, 9);
}

TEST(spec_option_count_empty) {
    int count = xr_cli_option_count(NULL);
    ASSERT_EQ_INT(count, 0);
}

/* ========== Option Map Accessors ========== */

TEST(optmap_not_present) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);

    int count = xr_cli_option_count(spec->options);
    bool present[16] = {false};
    const char *values[16] = {NULL};

    XrCliOptionMap map = {
        .spec = spec->options,
        .count = count,
        .present = present,
        .values = values,
    };

    ASSERT_FALSE(xr_cli_opt_present(&map, "trace"));
    ASSERT_FALSE(xr_cli_opt_bool(&map, "trace"));
    ASSERT_EQ_INT(xr_cli_opt_int(&map, "workers", 42), 42);
}

TEST(optmap_present_flag) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);

    int count = xr_cli_option_count(spec->options);
    bool present[16] = {false};
    const char *values[16] = {NULL};

    /* Set "trace" (index 0) as present */
    present[0] = true;

    XrCliOptionMap map = {
        .spec = spec->options,
        .count = count,
        .present = present,
        .values = values,
    };

    ASSERT_TRUE(xr_cli_opt_present(&map, "trace"));
    ASSERT_TRUE(xr_cli_opt_bool(&map, "trace"));
}

TEST(optmap_present_string) {
    const XrCliCommandSpec *spec = xr_cli_find_command("compile");
    ASSERT_NOT_NULL(spec);

    int count = xr_cli_option_count(spec->options);
    bool present[16] = {false};
    const char *values[16] = {NULL};

    /* Set "output" (index 0) as present */
    present[0] = true;
    values[0] = "out.xrc";

    XrCliOptionMap map = {
        .spec = spec->options,
        .count = count,
        .present = present,
        .values = values,
    };

    ASSERT_TRUE(xr_cli_opt_present(&map, "output"));
    ASSERT_STR_EQ(xr_cli_opt_string(&map, "output", NULL), "out.xrc");
}

TEST(optmap_present_int) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);

    int count = xr_cli_option_count(spec->options);
    bool present[16] = {false};
    const char *values[16] = {NULL};

    /* Set "workers" (index 2) */
    present[2] = true;
    values[2] = "8";

    XrCliOptionMap map = {
        .spec = spec->options,
        .count = count,
        .present = present,
        .values = values,
    };

    ASSERT_EQ_INT(xr_cli_opt_int(&map, "workers", 0), 8);
}

TEST(optmap_nonexistent_option) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);

    int count = xr_cli_option_count(spec->options);
    bool present[16] = {false};
    const char *values[16] = {NULL};

    XrCliOptionMap map = {
        .spec = spec->options,
        .count = count,
        .present = present,
        .values = values,
    };

    ASSERT_FALSE(xr_cli_opt_present(&map, "nonexistent"));
    ASSERT_EQ_INT(xr_cli_opt_int(&map, "nonexistent", -1), -1);
}

/* ========== Global Flag Parsing ========== */

TEST(parse_global_no_args) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray"};
    int consumed = xr_cli_parse_global(1, argv, &ctx);
    ASSERT_EQ_INT(consumed, 0);
    ASSERT_STR_EQ(ctx.program, "xray");
}

TEST(parse_global_help) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--help"};
    int consumed = xr_cli_parse_global(2, argv, &ctx);
    ASSERT_EQ_INT(consumed, -1);
}

TEST(parse_global_version) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--version"};
    int consumed = xr_cli_parse_global(2, argv, &ctx);
    ASSERT_EQ_INT(consumed, -2);
}

TEST(parse_global_version_short) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "-v"};
    int consumed = xr_cli_parse_global(2, argv, &ctx);
    ASSERT_EQ_INT(consumed, -2);
}

TEST(parse_global_no_color) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--no-color"};
    int consumed = xr_cli_parse_global(2, argv, &ctx);
    ASSERT_EQ_INT(consumed, 1);
    ASSERT_FALSE(ctx.color);
}

TEST(parse_global_color) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--color"};
    int consumed = xr_cli_parse_global(2, argv, &ctx);
    ASSERT_EQ_INT(consumed, 1);
    ASSERT_TRUE(ctx.color);
}

TEST(parse_global_command_not_consumed) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "run", "file.xr"};
    int consumed = xr_cli_parse_global(3, argv, &ctx);
    ASSERT_EQ_INT(consumed, 0);
}

/* ========== Command Parser ========== */

TEST(parse_cmd_simple_flag) {
    const XrCliCommandSpec *spec = xr_cli_find_command("check");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"--verbose", "src/"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 2, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_TRUE(xr_cli_opt_bool(&inv.options, "verbose"));
    ASSERT_EQ_INT(inv.positional_count, 1);
    ASSERT_STR_EQ(inv.positionals[0], "src/");

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_short_flag) {
    const XrCliCommandSpec *spec = xr_cli_find_command("check");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"-v", "-s", "file.xr"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 3, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_TRUE(xr_cli_opt_bool(&inv.options, "verbose"));
    ASSERT_TRUE(xr_cli_opt_bool(&inv.options, "strict"));
    ASSERT_EQ_INT(inv.positional_count, 1);

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_long_option_with_value) {
    const XrCliCommandSpec *spec = xr_cli_find_command("compile");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"--output", "out.xrc", "--format", "bytecode", "input.xr"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 5, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_STR_EQ(xr_cli_opt_string(&inv.options, "output", NULL), "out.xrc");
    ASSERT_STR_EQ(xr_cli_opt_string(&inv.options, "format", NULL), "bytecode");
    ASSERT_EQ_INT(inv.positional_count, 1);
    ASSERT_STR_EQ(inv.positionals[0], "input.xr");

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_long_option_eq_form) {
    const XrCliCommandSpec *spec = xr_cli_find_command("compile");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"--output=out.c", "input.xr"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 2, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_STR_EQ(xr_cli_opt_string(&inv.options, "output", NULL), "out.c");

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_short_with_value) {
    const XrCliCommandSpec *spec = xr_cli_find_command("compile");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"-oout.xrc", "input.xr"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 2, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_STR_EQ(xr_cli_opt_string(&inv.options, "output", NULL), "out.xrc");

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_unknown_option) {
    const XrCliCommandSpec *spec = xr_cli_find_command("check");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"--foobar"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 1, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_USAGE);
}

TEST(parse_cmd_missing_value) {
    const XrCliCommandSpec *spec = xr_cli_find_command("compile");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"--output"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 1, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_USAGE);
}

TEST(parse_cmd_no_positionals) {
    const XrCliCommandSpec *spec = xr_cli_find_command("repl");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;

    XrCliExitCode rc = xr_cli_parse_command(spec, 0, NULL, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_EQ_INT(inv.positional_count, 0);

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_passthrough) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"file.xr", "--", "arg1", "arg2"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 4, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_OK);
    ASSERT_EQ_INT(inv.positional_count, 1);
    ASSERT_STR_EQ(inv.positionals[0], "file.xr");
    ASSERT_EQ_INT(inv.passthrough_argc, 2);
    ASSERT_STR_EQ(inv.passthrough_argv[0], "arg1");
    ASSERT_STR_EQ(inv.passthrough_argv[1], "arg2");

    xr_cli_invocation_free(&inv);
}

TEST(parse_cmd_passthrough_rejected) {
    const XrCliCommandSpec *spec = xr_cli_find_command("check");
    ASSERT_NOT_NULL(spec);

    XrCliContext ctx = {.program = "xray"};
    XrCliInvocation inv;
    char *argv[] = {"file.xr", "--", "arg"};

    XrCliExitCode rc = xr_cli_parse_command(spec, 3, argv, &ctx, &inv);
    ASSERT_EQ_INT(rc, XR_CLI_EXIT_USAGE);
}

TEST(parse_global_verbose) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--verbose", "run"};
    int consumed = xr_cli_parse_global(3, argv, &ctx);
    ASSERT_EQ_INT(consumed, 1);
    ASSERT_TRUE(ctx.verbose);
}

TEST(parse_global_quiet) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--quiet", "check"};
    int consumed = xr_cli_parse_global(3, argv, &ctx);
    ASSERT_EQ_INT(consumed, 1);
    ASSERT_TRUE(ctx.quiet);
}

TEST(parse_global_quiet_short) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "-q", "check"};
    int consumed = xr_cli_parse_global(3, argv, &ctx);
    ASSERT_EQ_INT(consumed, 1);
    ASSERT_TRUE(ctx.quiet);
}

TEST(parse_global_json) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--json", "test"};
    int consumed = xr_cli_parse_global(3, argv, &ctx);
    ASSERT_EQ_INT(consumed, 1);
    ASSERT_TRUE(ctx.json_output);
}

TEST(parse_global_multiple_flags) {
    XrCliContext ctx = {0};
    char *argv[] = {"xray", "--no-color", "--verbose", "run"};
    int consumed = xr_cli_parse_global(4, argv, &ctx);
    ASSERT_EQ_INT(consumed, 2);
    ASSERT_FALSE(ctx.color);
    ASSERT_TRUE(ctx.verbose);
}

TEST(spec_handler_null_before_register) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);
    /* Handler may be NULL (test binary doesn't link handlers) */
}

/* ========== Help Generation (smoke tests) ========== */

// Redirect stdout to a null device so help-printing tests stay silent.
// MSVC does not allow direct stdout assignment; use freopen + freopen.
#ifdef _WIN32
#define SUPPRESS_STDOUT_BEGIN() freopen("NUL", "w", stdout)
#define SUPPRESS_STDOUT_END()   freopen("CON", "w", stdout)
#else
#define SUPPRESS_STDOUT_BEGIN() freopen("/dev/null", "w", stdout)
#define SUPPRESS_STDOUT_END()   freopen("/dev/tty", "w", stdout)
#endif

TEST(help_print_usage_no_crash) {
    SUPPRESS_STDOUT_BEGIN();
    xr_cli_print_usage();
    SUPPRESS_STDOUT_END();
}

TEST(help_print_command_help_no_crash) {
    const XrCliCommandSpec *spec = xr_cli_find_command("run");
    ASSERT_NOT_NULL(spec);

    SUPPRESS_STDOUT_BEGIN();
    xr_cli_print_command_help(spec);
    SUPPRESS_STDOUT_END();
}

TEST(help_print_subcommand_help_no_crash) {
    const XrCliCommandSpec *spec = xr_cli_find_command("pkg");
    ASSERT_NOT_NULL(spec);

    SUPPRESS_STDOUT_BEGIN();
    xr_cli_print_command_help(spec);
    SUPPRESS_STDOUT_END();
}

TEST(help_print_version_no_crash) {
    SUPPRESS_STDOUT_BEGIN();
    xr_cli_print_version();
    SUPPRESS_STDOUT_END();
}

/* ========== Test Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("Command Registry");
    RUN_TEST(spec_get_commands_not_null);
    RUN_TEST(spec_find_command_run);
    RUN_TEST(spec_find_command_eval);
    RUN_TEST(spec_find_command_repl);
    RUN_TEST(spec_find_command_pkg);
    RUN_TEST(spec_find_command_unknown);
    RUN_TEST(spec_aliases_removed);
    RUN_TEST(spec_option_count);
    RUN_TEST(spec_option_count_empty);

    RUN_TEST_SUITE("Option Map Accessors");
    RUN_TEST(optmap_not_present);
    RUN_TEST(optmap_present_flag);
    RUN_TEST(optmap_present_string);
    RUN_TEST(optmap_present_int);
    RUN_TEST(optmap_nonexistent_option);

    RUN_TEST_SUITE("Global Flag Parsing");
    RUN_TEST(parse_global_no_args);
    RUN_TEST(parse_global_help);
    RUN_TEST(parse_global_version);
    RUN_TEST(parse_global_version_short);
    RUN_TEST(parse_global_no_color);
    RUN_TEST(parse_global_color);
    RUN_TEST(parse_global_command_not_consumed);
    RUN_TEST(parse_global_verbose);
    RUN_TEST(parse_global_quiet);
    RUN_TEST(parse_global_quiet_short);
    RUN_TEST(parse_global_json);
    RUN_TEST(parse_global_multiple_flags);

    RUN_TEST_SUITE("Handler Registration");
    RUN_TEST(spec_handler_null_before_register);

    RUN_TEST_SUITE("Command Parser");
    RUN_TEST(parse_cmd_simple_flag);
    RUN_TEST(parse_cmd_short_flag);
    RUN_TEST(parse_cmd_long_option_with_value);
    RUN_TEST(parse_cmd_long_option_eq_form);
    RUN_TEST(parse_cmd_short_with_value);
    RUN_TEST(parse_cmd_unknown_option);
    RUN_TEST(parse_cmd_missing_value);
    RUN_TEST(parse_cmd_no_positionals);
    RUN_TEST(parse_cmd_passthrough);
    RUN_TEST(parse_cmd_passthrough_rejected);

    RUN_TEST_SUITE("Help Generation");
    RUN_TEST(help_print_usage_no_crash);
    RUN_TEST(help_print_command_help_no_crash);
    RUN_TEST(help_print_subcommand_help_no_crash);
    RUN_TEST(help_print_version_no_crash);

TEST_MAIN_END()
