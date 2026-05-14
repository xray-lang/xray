/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_repl.c - Xray REPL interactive environment
 *
 * KEY CONCEPT:
 *   - Multi-line input support (auto-detects unclosed brackets)
 *   - Rich built-in commands (.help, .clear, .load, .time, etc.)
 *   - History support (uses readline library)
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xcli_isolate.h"
#include "xcli_output.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../api/xrepl.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_fd.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef HAS_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define REPL_BUFFER_SIZE 8192
#define REPL_HISTORY_FILE ".xray_history"
#define REPL_MAX_HISTORY 1000

// Proto list for tracking compiled protos (freed on .reset / exit)
#define REPL_PROTO_INITIAL_CAP 32

typedef struct {
    XrayIsolate *isolate;
    char *buffer;  // Current input buffer
    size_t buffer_size;
    size_t buffer_len;
    bool use_color;    // Use color output
    XrProto **protos;  // Compiled protos (not freed per-input)
    int proto_count;
    int proto_capacity;
} ReplState;

#ifdef HAS_READLINE
/* ========== Tab Completion ========== */

/* REPL dot commands.  Kept here (rather than in a generic xr_commands
 * table) because completion is the only consumer and we want the same
 * file to host the dispatcher and the suggester — keeps them in sync. */
static const char *const k_repl_dot_commands[] = {
    ".help", ".exit",    ".quit", ".clear", ".reset", ".load",
    ".time", ".history", ".vars", ".type",  NULL,
};

/* Subset of xray keywords useful at REPL top level.  Kept local to
 * avoid cross-app coupling with src/app/lsp.  Order is not significant
 * — readline displays matches alphabetically. */
static const char *const k_repl_keywords[] = {
    "let",      "const",         "fn",          "class",        "interface", "enum",      "type",
    "if",       "else",          "while",       "for",          "in",        "is",        "break",
    "continue", "return",        "match",       "true",         "false",     "null",      "import",
    "export",   "from",          "as",          "go",           "await",     "select",    "defer",
    "scope",    "after",         "try",         "catch",        "finally",   "throw",     "new",
    "this",     "super",         "extends",     "implements",   "static",    "private",   "public",
    "abstract", "override",      "operator",    "void",         "int",       "float",     "string",
    "bool",     "Array",         "Map",         "Set",          "Json",      "Channel",   "Bytes",
    "BigInt",   "StringBuilder", "Exception",   "Regex",        "print",     "dump",      "typeof",
    "typename", "assert",        "assert_true", "assert_false", "assert_eq", "assert_ne", "copy",
    "chr",      "Coro",          "CoroPool",    "Reflect",      "Type",      NULL,
};

/* Set during repl_run() so the readline generator can reach the
 * current ReplState (and through it, the isolate's repl_symbols).
 * Readline's generator signature has no userdata pointer, so a
 * file-scope static is the conventional approach.  Cleared at exit. */
static ReplState *g_completion_state = NULL;

/* readline word generator.
 * `state` is 0 on the first call for a given word, then nonzero for
 * subsequent calls until the generator returns NULL (signals "no more
 * matches").  We pass each candidate list in turn: dot commands first
 * iff the word starts with '.', then keywords / builtins, then live
 * REPL bindings. */
static char *repl_completion_generator(const char *text, int state) {
    static int phase;     /* 0 = dots, 1 = keywords, 2 = repl symbols */
    static int idx;       /* index within the current phase list */
    static size_t len;    /* cached strlen(text) for prefix match */
    static bool dot_only; /* true when text begins with '.' */

    if (state == 0) {
        phase = 0;
        idx = 0;
        len = strlen(text);
        dot_only = (len > 0 && text[0] == '.');
    }

    /* Phase 0: dot commands (always scanned; cheap and tiny). */
    if (phase == 0) {
        while (k_repl_dot_commands[idx]) {
            const char *cmd = k_repl_dot_commands[idx++];
            if (strncmp(cmd, text, len) == 0)
                return xr_strdup(cmd);
        }
        phase = 1;
        idx = 0;
    }

    /* If the user is clearly typing a dot command, do not pollute
     * matches with keywords / symbols. */
    if (dot_only)
        return NULL;

    /* Phase 1: keywords + builtins. */
    if (phase == 1) {
        while (k_repl_keywords[idx]) {
            const char *kw = k_repl_keywords[idx++];
            if (strncmp(kw, text, len) == 0)
                return xr_strdup(kw);
        }
        phase = 2;
        idx = 0;
    }

    /* Phase 2: user-defined top-level REPL bindings. */
    if (phase == 2 && g_completion_state && g_completion_state->isolate) {
        XrReplSymbolTable *table = xr_repl_symbols_of(g_completion_state->isolate);
        if (table) {
            while (idx < table->count) {
                const XrReplSymbol *sym = &table->symbols[idx++];
                const char *name = xr_repl_symbol_cname(sym);
                if (!name)
                    continue;
                if (strncmp(name, text, len) == 0)
                    return xr_strdup(name);
            }
        }
    }

    return NULL;
}

static char **repl_completion(const char *text, int start, int end) {
    (void) start;
    (void) end;
    /* Disable readline's default filename completion fallback so that
     * unmatched words simply offer no suggestions rather than listing
     * cwd contents (which is almost never what a REPL user wants). */
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, repl_completion_generator);
}
#endif  // HAS_READLINE

// Global flag for SIGINT handling
static volatile sig_atomic_t g_repl_interrupted = 0;

static void repl_sigint_handler(int sig) {
    (void) sig;
    g_repl_interrupted = 1;
}

// Check if terminal supports color
static bool terminal_supports_color(void) {
    const char *term = getenv("TERM");
    if (!term)
        return false;
    if (strcmp(term, "dumb") == 0)
        return false;
    return xr_isatty(xr_stdout_fd());
}

// Get history file path
static char *get_history_file(void) {
    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char *path = (char *) xr_malloc(512);
    snprintf(path, 512, "%s/%s", home, REPL_HISTORY_FILE);
    return path;
}

#define get_time_ms() xr_cli_get_time_ms()

// Print colored text
static void print_colored(ReplState *state, const char *color, const char *text) {
    if (state->use_color) {
        printf("%s%s%s", color, text, XR_CLR_RESET);
    } else {
        printf("%s", text);
    }
}

// Print welcome message
static void print_welcome(ReplState *state) {
    if (state->use_color) {
        printf("\n");
        printf("  Welcome to %s%sXray%s %sv%s%s\n", XR_CLR_BOLD, XR_CLR_CYAN, XR_CLR_RESET,
               XR_CLR_DIM, XRAY_VERSION_STRING, XR_CLR_RESET);
        printf("  %sType %s.help%s%s for commands, %s.exit%s%s to quit, "
               "%sTab%s%s to complete%s\n",
               XR_CLR_DIM, XR_CLR_RESET, XR_CLR_BOLD, XR_CLR_DIM, XR_CLR_RESET, XR_CLR_BOLD,
               XR_CLR_DIM, XR_CLR_RESET, XR_CLR_BOLD, XR_CLR_DIM, XR_CLR_RESET);
        printf("\n");
    } else {
        printf("\n");
        printf("  Welcome to Xray v%s\n", XRAY_VERSION_STRING);
        printf("  Type .help for commands, .exit to quit, Tab to complete\n");
        printf("\n");
    }
}

// Print brief help
static void print_help_brief(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Commands\n");
    printf("  .help              Show this help\n");
    printf("  .exit  .quit       Exit REPL\n");
    printf("  .clear             Clear screen\n");
    printf("  .reset             Reset environment\n");
    printf("  .load <file>       Load and execute file\n");
    printf("  .time <expr>       Measure execution time\n");
    printf("  .vars              Show all bindings\n");
    printf("  .type <expr>       Show type of expression\n");
#ifdef HAS_READLINE
    printf("  .history           Show recent history\n");
#endif
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Keyboard\n");
    printf("  Tab      Complete    Ctrl+C   Cancel\n");
    printf("  Ctrl+D   Exit        Ctrl+L   Clear\n");
    printf("\n");
    print_colored(state, XR_CLR_DIM, "Topics: ");
    printf(".help syntax  .help types  .help coro  .help operators\n\n");
}

// Print syntax help
static void print_help_syntax(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Basic Syntax:\n");
    printf("  let x = 1              // mutable variable\n");
    printf("  const PI = 3.14        // constant\n");
    printf("  fn add(a, b) { a + b } // function\n");
    printf("  (x) => x * 2           // arrow function\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Control Flow:\n");
    printf("  if (x > 0) { } else { }\n");
    printf("  for (let i = 0; i < 10; i++) { }\n");
    printf("  for (item in array) { }\n");
    printf("  while (cond) { }\n");
    printf("  match x { 1 => \"one\", _ => \"other\" }\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Class:\n");
    printf("  class Point {\n");
    printf("    x = 0; y = 0\n");
    printf("    constructor(x, y) { this.x = x; this.y = y }\n");
    printf("    distance() { (this.x**2 + this.y**2)**0.5 }\n");
    printf("  }\n");
    printf("\n");
}

// Print types help
static void print_help_types(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Basic Types:\n");
    printf("  int, float, string, bool, null\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Container Types:\n");
    printf("  Array       [1, 2, 3]\n");
    printf("  Map         {\"a\" => 1, \"b\" => 2}\n");
    printf("  Set         #[1, 2, 3]\n");
    printf("  Json        {name: \"xray\", version: 1}\n");
    printf("  Bytes       Bytes(1024)\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Type Annotations:\n");
    printf("  let x: int = 1\n");
    printf("  let arr: Array<int> = [1, 2]\n");
    printf("  fn add(a: int, b: int): int { a + b }\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Type Conversion:\n");
    printf("  int(3.14)      // 3\n");
    printf("  float(\"3.14\")  // 3.14\n");
    printf("  string(123)    // \"123\"\n");
    printf("  typeof(x)      // \"int\"\n");
    printf("\n");
}

// Print coroutine help
static void print_help_coro(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Coroutine Basics:\n");
    printf("  let t = go func()      // spawn coroutine\n");
    printf("  await t                // wait for completion\n");
    printf("  yield                  // yield execution\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Channel:\n");
    printf("  const ch = Channel()   // unbuffered\n");
    printf("  const ch = Channel(10) // buffered\n");
    printf("  ch.send(value)         // blocking send\n");
    printf("  let v = ch.recv()      // blocking receive\n");
    printf("  ch.close()             // close channel\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Select:\n");
    printf("  select {\n");
    printf("    msg from ch1 => { print(msg) }\n");
    printf("    ch2.send(v)  => { print(\"sent\") }\n");
    printf("    after 1000   => { print(\"timeout\") }\n");
    printf("  }\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Shared Variables:\n");
    printf("  shared const CFG = {...}  // read-only sharing\n");
    printf("  shared let data = [...]   // move semantics\n");
    printf("\n");
}

// Print operators help
static void print_help_operators(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Arithmetic:\n");
    printf("  +  -  *  /  %%  **    // ** is power\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Comparison:\n");
    printf("  ==  !=  <  >  <=  >=  // value comparison\n");
    printf("  ===  !==              // reference comparison\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Logical:\n");
    printf("  &&  ||  !             // and, or, not\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Bitwise:\n");
    printf("  &  |  ^  ~  <<  >>    // bit operations\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Special:\n");
    printf("  a ?? b                // null coalescing\n");
    printf("  obj?.prop             // optional chaining\n");
    printf("  a ? b : c             // ternary\n");
    printf("  0..10                 // range\n");
    printf("  arr[1:3]              // slice\n");
    printf("\n");
}

// Print help (dispatch to specific topic)
static void print_help(ReplState *state, const char *topic) {
    if (!topic || strlen(topic) == 0) {
        print_help_brief(state);
        return;
    }

    // Skip leading spaces
    while (*topic == ' ')
        topic++;

    if (strcmp(topic, "syntax") == 0) {
        print_help_syntax(state);
    } else if (strcmp(topic, "types") == 0) {
        print_help_types(state);
    } else if (strcmp(topic, "coro") == 0 || strcmp(topic, "coroutine") == 0) {
        print_help_coro(state);
    } else if (strcmp(topic, "operators") == 0 || strcmp(topic, "ops") == 0) {
        print_help_operators(state);
    } else {
        print_colored(state, XR_CLR_RED, "Unknown topic: ");
        printf("%s\n", topic);
        printf("Available topics: syntax, types, coro, operators\n\n");
    }
}

// Clear screen
static void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

// Check if input is structurally complete using the lexer.
// Delegates to xr_repl_check_input() which scans tokens and tracks bracket depth.
static bool is_input_complete(ReplState *state) {
    return xr_repl_check_input(state->buffer) == XR_INPUT_COMPLETE;
}

// Append to buffer
static void append_to_buffer(ReplState *state, const char *line) {
    size_t len = strlen(line);

    // Ensure capacity
    if (state->buffer_len + len + 2 >= state->buffer_size) {
        state->buffer_size *= 2;
        state->buffer = (char *) xr_realloc(state->buffer, state->buffer_size);
    }

    // Append
    if (state->buffer_len > 0) {
        state->buffer[state->buffer_len++] = '\n';
    }
    memcpy(state->buffer + state->buffer_len, line, len);
    state->buffer_len += len;
    state->buffer[state->buffer_len] = '\0';
}

// Reset buffer
static void reset_buffer(ReplState *state) {
    state->buffer_len = 0;
    state->buffer[0] = '\0';
}

// Incremental execution: compile only new code, execute on persistent isolate+runtime.
// Definitions survive across inputs via REPL symbol table + shared array.
// Runtime stays alive across inputs (GC objects like closures must not be freed).
// Free all tracked protos
static void repl_free_protos(ReplState *state) {
    for (int i = 0; i < state->proto_count; i++) {
        xr_free_code(state->isolate, state->protos[i]);
    }
    state->proto_count = 0;
}

static void execute_code(ReplState *state, const char *code) {
    // Incremental compile (seeds compiler context from repl_symbols)
    XrProto *proto = xr_repl_compile(state->isolate, code);
    if (!proto) {
        return;  // compile error already reported
    }

    // Track proto for later cleanup (closures reference sub-protos)
    if (state->proto_count >= state->proto_capacity) {
        state->proto_capacity *= 2;
        XR_REALLOC_OR_ABORT(state->protos, (size_t) state->proto_capacity * sizeof(XrProto *),
                            "repl protos grow");
    }
    state->protos[state->proto_count++] = proto;

    // Execute on persistent runtime
    xr_execute(state->isolate, proto);
}

// Handle .load command (uses incremental compilation path)
static void cmd_load(ReplState *state, const char *filename) {
    if (!filename || strlen(filename) == 0) {
        print_colored(state, XR_CLR_RED, "Error: please specify filename\n");
        printf("Usage: .load <filename>\n");
        return;
    }

    // Skip leading spaces
    while (*filename == ' ')
        filename++;

    char *source = xr_cli_read_file(filename);
    if (!source) {
        print_colored(state, XR_CLR_RED, "Error: ");
        printf("cannot open file '%s'\n", filename);
        return;
    }

    printf("Loading %s...\n", filename);
    execute_code(state, source);
    xr_free(source);
}

// Handle .time command (uses incremental compilation path)
static void cmd_time(ReplState *state, const char *expr) {
    if (!expr || strlen(expr) == 0) {
        print_colored(state, XR_CLR_RED, "Error: please specify expression\n");
        printf("Usage: .time <expression>\n");
        return;
    }

    // Skip leading spaces
    while (*expr == ' ')
        expr++;

    double start = get_time_ms();
    execute_code(state, expr);
    double end = get_time_ms();

    printf("%s%.3f ms%s\n", state->use_color ? XR_CLR_GRAY : "", end - start,
           state->use_color ? XR_CLR_RESET : "");
}

// Handle built-in commands
// Returns: true = handled, false = not a built-in command
static bool handle_command(ReplState *state, const char *input) {
    // .exit / .quit
    if (strcmp(input, ".exit") == 0 || strcmp(input, ".quit") == 0) {
        return false;  // Special handling, exit from main loop
    }

    // .help [topic]
    if (strcmp(input, ".help") == 0) {
        print_help(state, NULL);
        return true;
    }
    if (strncmp(input, ".help ", 6) == 0) {
        print_help(state, input + 6);
        return true;
    }

    // .clear
    if (strcmp(input, ".clear") == 0) {
        clear_screen();
        print_welcome(state);
        return true;
    }

    // .reset
    if (strcmp(input, ".reset") == 0) {
        repl_free_protos(state);
        xr_multicore_destroy(state->isolate);
        xray_isolate_delete(state->isolate);
        state->isolate = xr_cli_isolate_new(XR_CLI_ISOLATE_REPL);
        if (!state->isolate) {
            fprintf(stderr, "Error: failed to create isolate\n");
            return true;
        }
        xr_multicore_init(state->isolate, 0);
        print_colored(state, XR_CLR_GREEN, "Environment reset\n");
        return true;
    }

    // .load <file>
    if (strncmp(input, ".load", 5) == 0) {
        cmd_load(state, input + 5);
        return true;
    }

    // .time <expr>
    if (strncmp(input, ".time", 5) == 0) {
        cmd_time(state, input + 5);
        return true;
    }

    // .vars — list top-level REPL bindings
    if (strcmp(input, ".vars") == 0) {
        xr_repl_print_vars(state->isolate);
        return true;
    }

    // .type <expr> — show runtime type of an expression
    if (strncmp(input, ".type", 5) == 0 && (input[5] == '\0' || input[5] == ' ')) {
        xr_repl_print_type(state->isolate, input + 5);
        return true;
    }

#ifdef HAS_READLINE
    // .history
    if (strcmp(input, ".history") == 0) {
        int len = history_length;
        int start = len > 20 ? len - 20 : 1;
        for (int i = start; i <= len; i++) {
            HIST_ENTRY *entry = history_get(i);
            if (entry) {
                printf("%s%4d%s  %s\n", state->use_color ? XR_CLR_GRAY : "", i,
                       state->use_color ? XR_CLR_RESET : "", entry->line);
            }
        }
        return true;
    }
#endif

    // Unknown command
    if (input[0] == '.') {
        print_colored(state, XR_CLR_RED, "Unknown command: ");
        printf("%s\n", input);
        printf("Type .help for available commands\n");
        return true;
    }

    return false;
}

// Get prompt
// readline requires non-printing chars (ANSI escapes) wrapped in \001..\002
// so it can correctly calculate visible prompt width for cursor positioning.
static const char *get_prompt(ReplState *state, bool is_continuation) {
    static char prompt[128];

    if (is_continuation) {
        if (state->use_color) {
            snprintf(prompt, sizeof(prompt),
                     "\001" XR_CLR_GRAY "\002"
                     "....."
                     "\001" XR_CLR_RESET "\002"
                     " ");
        } else {
            snprintf(prompt, sizeof(prompt), "..... ");
        }
    } else {
        if (state->use_color) {
            snprintf(prompt, sizeof(prompt),
                     "\001" XR_CLR_BLUE "\002"
                     "xray>"
                     "\001" XR_CLR_RESET "\002"
                     " ");
        } else {
            snprintf(prompt, sizeof(prompt), "xray> ");
        }
    }

    return prompt;
}

// Read one line of input
static char *read_line(ReplState *state, bool is_continuation) {
    const char *prompt = get_prompt(state, is_continuation);

#ifdef HAS_READLINE
    char *line = readline(prompt);
    /* History add is deferred to the main loop, which calls
     * add_history(state.buffer) once the full multi-line input is
     * structurally complete.  Pressing ↑ then retrieves the entire
     * block in one go, which readline displays and re-edits sanely
     * even across newlines. */
    return line;
#else
    printf("%s", prompt);
    fflush(stdout);

    static char buffer[REPL_BUFFER_SIZE];
    if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
        return NULL;
    }

    // Remove newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }

    return xr_strdup(buffer);
#endif
}

XR_FUNC int cmd_repl(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    ReplState state = {0};
    state.use_color = terminal_supports_color();
    if (xr_cli_opt_bool(&inv->options, "no-color"))
        state.use_color = false;

    /* Initialize buffer */
    state.buffer_size = REPL_BUFFER_SIZE;
    state.buffer = (char *) xr_malloc(state.buffer_size);
    state.buffer[0] = '\0';
    state.buffer_len = 0;

    // Initialize proto tracking
    state.proto_capacity = REPL_PROTO_INITIAL_CAP;
    state.protos = (XrProto **) xr_malloc(state.proto_capacity * sizeof(XrProto *));
    state.proto_count = 0;

    // Setup SIGINT handler
    signal(SIGINT, repl_sigint_handler);

    // Load history
#ifdef HAS_READLINE
    char *history_file = get_history_file();
    if (history_file) {
        read_history(history_file);
    }
    /* Wire tab completion.  Set the generator and make `g_completion_state`
     * point at the live ReplState so phase-2 completion can see live
     * REPL bindings.  Cleared after the loop exits below. */
    g_completion_state = &state;
    rl_attempted_completion_function = repl_completion;
    /* Single-char word break set: anything that ends an identifier.
     * Notably keeps `.` so dot commands complete as a unit, but allows
     * space, parens, operators to bound a word. */
    rl_basic_word_break_characters = (char *) " \t\n\"\\'`@$><=;|&{(";
#endif

    // Create Isolate + Runtime (persistent across inputs for GC safety)
    state.isolate = xr_cli_isolate_new(XR_CLI_ISOLATE_REPL);
    if (!state.isolate) {
        xr_cli_error("repl", "failed to create isolate");
        xr_free(state.buffer);
        xr_free(state.protos);
        return XR_CLI_EXIT_INTERNAL;
    }
    xr_multicore_init(state.isolate, 0);

    // Print welcome
    print_welcome(&state);

    // REPL main loop
    while (1) {
        // Check interrupt flag
        if (g_repl_interrupted) {
            g_repl_interrupted = 0;
            if (state.buffer_len > 0) {
                printf("\n");
                reset_buffer(&state);
            } else {
                printf("\n");
            }
        }

        bool is_continuation = state.buffer_len > 0;
        char *line = read_line(&state, is_continuation);

        // Check interrupt during read
        if (g_repl_interrupted) {
            g_repl_interrupted = 0;
            xr_free(line);
            printf("\n");
            reset_buffer(&state);
            continue;
        }

        // EOF (Ctrl+D)
        if (!line) {
            if (state.buffer_len > 0) {
                printf("\n");
                reset_buffer(&state);
                continue;
            }
            printf("\n");
            break;
        }

        // Empty line
        if (strlen(line) == 0) {
            if (state.buffer_len > 0) {
                append_to_buffer(&state, "");
            }
            xr_free(line);
            continue;
        }

        // Check exit command
        if (state.buffer_len == 0 && (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0)) {
            xr_free(line);
            break;
        }

        // Handle built-in commands (only in non-multi-line mode)
        if (state.buffer_len == 0 && line[0] == '.') {
            handle_command(&state, line);
            xr_free(line);
            continue;
        }

        // Append to buffer
        append_to_buffer(&state, line);
        xr_free(line);

        // Check if input is complete
        if (is_input_complete(&state)) {
#ifdef HAS_READLINE
            /* Record the whole buffer (possibly multi-line) as one
             * history entry.  Empty buffers are skipped so users do
             * not see blank entries when they press Enter on an
             * empty prompt. */
            if (state.buffer_len > 0) {
                add_history(state.buffer);
            }
#endif
            execute_code(&state, state.buffer);
            reset_buffer(&state);
        }
    }

    // Save history
#ifdef HAS_READLINE
    if (history_file) {
        write_history(history_file);
        xr_free(history_file);
    }
    g_completion_state = NULL;
    rl_attempted_completion_function = NULL;
#endif

    // Restore default SIGINT handler
    signal(SIGINT, SIG_DFL);

    // Cleanup
    if (state.use_color)
        printf("%sbye%s\n", XR_CLR_DIM, XR_CLR_RESET);
    else
        printf("bye\n");
    repl_free_protos(&state);
    xr_multicore_destroy(state.isolate);
    xray_isolate_delete(state.isolate);
    xr_free(state.protos);
    xr_free(state.buffer);

    return XR_CLI_EXIT_OK;
}
