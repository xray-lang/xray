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
#include "xcli_utils.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../api/xrepl.h"
#include "../../runtime/xisolate_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#include "../../base/xmalloc.h"

#ifdef HAS_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define REPL_BUFFER_SIZE    8192
#define REPL_HISTORY_FILE   ".xray_history"
#define REPL_MAX_HISTORY    1000


// Proto list for tracking compiled protos (freed on .reset / exit)
#define REPL_PROTO_INITIAL_CAP 32

typedef struct {
    XrayIsolate *isolate;
    char *buffer;               // Current input buffer
    size_t buffer_size;
    size_t buffer_len;
    bool use_color;             // Use color output
    XrProto **protos;           // Compiled protos (not freed per-input)
    int proto_count;
    int proto_capacity;
} ReplState;

// Global flag for SIGINT handling
static volatile sig_atomic_t g_repl_interrupted = 0;

static void repl_sigint_handler(int sig) {
    (void)sig;
    g_repl_interrupted = 1;
}

// Check if terminal supports color
static bool terminal_supports_color(void) {
    const char *term = getenv("TERM");
    if (!term) return false;
    if (strcmp(term, "dumb") == 0) return false;
    return isatty(STDOUT_FILENO);
}

// Get history file path
static char* get_history_file(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;

    char *path = (char*)xr_malloc(512);
    snprintf(path, 512, "%s/%s", home, REPL_HISTORY_FILE);
    return path;
}

// Use cli_get_time_ms from cli_utils.h (monotonic clock)
#define get_time_ms() cli_get_time_ms()

// Print colored text
static void print_colored(ReplState *state, const char *color, const char *text) {
    if (state->use_color) {
        printf("%s%s%s", color, text, CLR_RESET);
    } else {
        printf("%s", text);
    }
}

// Print welcome message
static void print_welcome(ReplState *state) {
    printf("\n");

    if (state->use_color) {
        printf("  %s----------------------------------------%s\n", CLR_BLUE, CLR_RESET);
        printf("  %s%s* Xray%s %sv%d.%d.%d%s\n",
               CLR_BOLD, CLR_CYAN, CLR_RESET,
               CLR_GRAY, XRAY_VERSION_MAJOR, XRAY_VERSION_MINOR, XRAY_VERSION_PATCH, CLR_RESET);
        printf("  %s----------------------------------------%s\n", CLR_BLUE, CLR_RESET);
        printf("  %sType .help for commands, .exit to quit%s\n",
               CLR_GRAY, CLR_RESET);
    } else {
        printf("  ----------------------------------------\n");
        printf("  * Xray v%d.%d.%d\n", XRAY_VERSION_MAJOR, XRAY_VERSION_MINOR, XRAY_VERSION_PATCH);
        printf("  ----------------------------------------\n");
        printf("  Type .help for commands, .exit to quit\n");
    }

    printf("\n");
}

// Print brief help
static void print_help_brief(ReplState *state) {
    printf("\n");
    print_colored(state, CLR_BOLD, "REPL Commands:\n");
    printf("  .help            Show this help\n");
    printf("  .exit, .quit     Exit REPL\n");
    printf("  .clear           Clear screen\n");
    printf("  .reset           Reset environment\n");
    printf("  .load <file>     Load and execute file\n");
    printf("  .time <expr>     Time expression execution\n");
#ifdef HAS_READLINE
    printf("  .history         Show command history\n");
#endif
    printf("\n");
    print_colored(state, CLR_BOLD, "Shortcuts:\n");
    printf("  Ctrl+C  Cancel    Ctrl+D  Exit    Ctrl+L  Clear\n");
    printf("\n");
    print_colored(state, CLR_GRAY, "Type .help <topic> for details: ");
    printf("syntax, types, coro, operators\n\n");
}

// Print syntax help
static void print_help_syntax(ReplState *state) {
    printf("\n");
    print_colored(state, CLR_BOLD, "Basic Syntax:\n");
    printf("  let x = 1              // mutable variable\n");
    printf("  const PI = 3.14        // constant\n");
    printf("  fn add(a, b) { a + b } // function\n");
    printf("  (x) => x * 2           // arrow function\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Control Flow:\n");
    printf("  if (x > 0) { } else { }\n");
    printf("  for (let i = 0; i < 10; i++) { }\n");
    printf("  for (item in array) { }\n");
    printf("  while (cond) { }\n");
    printf("  match x { 1 => \"one\", _ => \"other\" }\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Class:\n");
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
    print_colored(state, CLR_BOLD, "Basic Types:\n");
    printf("  int, float, string, bool, null\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Container Types:\n");
    printf("  Array       [1, 2, 3]\n");
    printf("  Map         {\"a\" => 1, \"b\" => 2}\n");
    printf("  Set         #[1, 2, 3]\n");
    printf("  Bytes       Bytes(1024)\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Type Annotations:\n");
    printf("  let x: int = 1\n");
    printf("  let arr: Array<int> = [1, 2]\n");
    printf("  fn add(a: int, b: int): int { a + b }\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Type Conversion:\n");
    printf("  int(3.14)      // 3\n");
    printf("  float(\"3.14\")  // 3.14\n");
    printf("  string(123)    // \"123\"\n");
    printf("  typeof(x)      // \"int\"\n");
    printf("\n");
}

// Print coroutine help
static void print_help_coro(ReplState *state) {
    printf("\n");
    print_colored(state, CLR_BOLD, "Coroutine Basics:\n");
    printf("  let t = go func()      // spawn coroutine\n");
    printf("  await t                // wait for completion\n");
    printf("  yield                  // yield execution\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Channel:\n");
    printf("  const ch = Channel()   // unbuffered\n");
    printf("  const ch = Channel(10) // buffered\n");
    printf("  ch.send(value)         // blocking send\n");
    printf("  let v = ch.recv()      // blocking receive\n");
    printf("  ch.close()             // close channel\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Select:\n");
    printf("  select {\n");
    printf("    msg from ch1 => { print(msg) }\n");
    printf("    ch2.send(v)  => { print(\"sent\") }\n");
    printf("    after 1000   => { print(\"timeout\") }\n");
    printf("  }\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Shared Variables:\n");
    printf("  shared const CFG = {...}  // read-only sharing\n");
    printf("  shared let data = [...]   // move semantics\n");
    printf("\n");
}

// Print operators help
static void print_help_operators(ReplState *state) {
    printf("\n");
    print_colored(state, CLR_BOLD, "Arithmetic:\n");
    printf("  +  -  *  /  %%  **    // ** is power\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Comparison:\n");
    printf("  ==  !=  <  >  <=  >=  // value comparison\n");
    printf("  ===  !==              // reference comparison\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Logical:\n");
    printf("  &&  ||  !             // and, or, not\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Bitwise:\n");
    printf("  &  |  ^  ~  <<  >>    // bit operations\n");
    printf("\n");
    print_colored(state, CLR_BOLD, "Special:\n");
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
    while (*topic == ' ') topic++;

    if (strcmp(topic, "syntax") == 0) {
        print_help_syntax(state);
    } else if (strcmp(topic, "types") == 0) {
        print_help_types(state);
    } else if (strcmp(topic, "coro") == 0 || strcmp(topic, "coroutine") == 0) {
        print_help_coro(state);
    } else if (strcmp(topic, "operators") == 0 || strcmp(topic, "ops") == 0) {
        print_help_operators(state);
    } else {
        print_colored(state, CLR_RED, "Unknown topic: ");
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
        state->buffer = (char*)xr_realloc(state->buffer, state->buffer_size);
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
        XR_REALLOC_OR_ABORT(state->protos,
                            (size_t)state->proto_capacity * sizeof(XrProto*),
                            "repl protos grow");
    }
    state->protos[state->proto_count++] = proto;

    // Execute on persistent runtime
    xr_execute(state->isolate, proto);
}

// Handle .load command (uses incremental compilation path)
static void cmd_load(ReplState *state, const char *filename) {
    if (!filename || strlen(filename) == 0) {
        print_colored(state, CLR_RED, "Error: please specify filename\n");
        printf("Usage: .load <filename>\n");
        return;
    }

    // Skip leading spaces
    while (*filename == ' ') filename++;

    char *source = cli_read_file(filename);
    if (!source) {
        print_colored(state, CLR_RED, "Error: ");
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
        print_colored(state, CLR_RED, "Error: please specify expression\n");
        printf("Usage: .time <expression>\n");
        return;
    }

    // Skip leading spaces
    while (*expr == ' ') expr++;

    double start = get_time_ms();
    execute_code(state, expr);
    double end = get_time_ms();

    printf("%s%.3f ms%s\n",
           state->use_color ? CLR_GRAY : "",
           end - start,
           state->use_color ? CLR_RESET : "");
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
        state->isolate = cli_create_isolate();
        if (!state->isolate) {
            fprintf(stderr, "Error: failed to create isolate\n");
            return true;
        }
        xr_multicore_init(state->isolate, 0);
        print_colored(state, CLR_GREEN, "Environment reset\n");
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

#ifdef HAS_READLINE
    // .history
    if (strcmp(input, ".history") == 0) {
        int len = history_length;
        int start = len > 20 ? len - 20 : 1;
        for (int i = start; i <= len; i++) {
            HIST_ENTRY *entry = history_get(i);
            if (entry) {
                printf("%s%4d%s  %s\n",
                       state->use_color ? CLR_GRAY : "",
                       i,
                       state->use_color ? CLR_RESET : "",
                       entry->line);
            }
        }
        return true;
    }
#endif

    // Unknown command
    if (input[0] == '.') {
        print_colored(state, CLR_RED, "Unknown command: ");
        printf("%s\n", input);
        printf("Type .help for available commands\n");
        return true;
    }

    return false;
}

// Get prompt
// readline requires non-printing chars (ANSI escapes) wrapped in \001..\002
// so it can correctly calculate visible prompt width for cursor positioning.
static const char* get_prompt(ReplState *state, bool is_continuation) {
    static char prompt[128];

    if (is_continuation) {
        if (state->use_color) {
            snprintf(prompt, sizeof(prompt),
                     "\001" CLR_GRAY "\002" "....." "\001" CLR_RESET "\002" " ");
        } else {
            snprintf(prompt, sizeof(prompt), "..... ");
        }
    } else {
        if (state->use_color) {
            snprintf(prompt, sizeof(prompt),
                     "\001" CLR_BLUE "\002" "xray>" "\001" CLR_RESET "\002" " ");
        } else {
            snprintf(prompt, sizeof(prompt), "xray> ");
        }
    }

    return prompt;
}

// Read one line of input
static char* read_line(ReplState *state, bool is_continuation) {
    const char *prompt = get_prompt(state, is_continuation);

#ifdef HAS_READLINE
    char *line = readline(prompt);
    // Only add single-line (non-continuation) input to history here.
    // Multi-line input is added as a whole after completion in the main loop.
    if (!is_continuation && line && *line) {
        add_history(line);
    }
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
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }

    return xr_strdup(buffer);
#endif
}

void print_repl_help(void) {
    printf("Usage: xray [options]\n");
    printf("\n");
    printf("Start interactive REPL (Read-Eval-Print-Loop)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --no-color      Disable color output\n");
    printf("  -h, --help      Show this help\n");
    printf("\n");
    printf("REPL Commands:\n");
    printf("  .help           Show help\n");
    printf("  .exit, .quit    Exit\n");
    printf("  .clear          Clear screen\n");
    printf("  .reset          Reset environment\n");
    printf("  .load <file>    Load file\n");
    printf("  .time <expr>    Time execution\n");
    printf("\n");
}

static struct option repl_long_options[] = {
    {"no-color", no_argument, 0, 'n'},
    {"help",     no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

int cmd_repl(int argc, char **argv) {
    ReplState state = {0};
    state.use_color = terminal_supports_color();

    // Parse arguments with getopt_long
    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "nh", repl_long_options, NULL)) != -1) {
        switch (opt) {
            case 'n': state.use_color = false; break;
            case 'h': print_repl_help(); return 0;
            default: print_repl_help(); return 1;
        }
    }

    // Initialize buffer
    state.buffer_size = REPL_BUFFER_SIZE;
    state.buffer = (char*)xr_malloc(state.buffer_size);
    state.buffer[0] = '\0';
    state.buffer_len = 0;

    // Initialize proto tracking
    state.proto_capacity = REPL_PROTO_INITIAL_CAP;
    state.protos = (XrProto**)xr_malloc(state.proto_capacity * sizeof(XrProto*));
    state.proto_count = 0;

    // Setup SIGINT handler
    struct sigaction sa;
    sa.sa_handler = repl_sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // Load history
#ifdef HAS_READLINE
    char *history_file = get_history_file();
    if (history_file) {
        read_history(history_file);
    }
#endif

    // Create Isolate + Runtime (persistent across inputs for GC safety)
    state.isolate = cli_create_isolate();
    if (!state.isolate) {
        fprintf(stderr, "Error: cannot create execution environment\n");
        xr_free(state.buffer);
        return 1;
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
        if (state.buffer_len == 0 &&
            (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0)) {
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
#endif

    // Restore default SIGINT handler
    signal(SIGINT, SIG_DFL);

    // Cleanup
    printf("Bye!\n");
    repl_free_protos(&state);
    xr_multicore_destroy(state.isolate);
    xray_isolate_delete(state.isolate);
    xr_free(state.protos);
    xr_free(state.buffer);

    return 0;
}
