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
#include "xcli_output.h"
#include "xray_source_repl.h"
#include "xray_version.h"
#include "../../execution/xr_xir_host_time.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../os/os_fd.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#ifdef XR_OS_WINDOWS
#if defined(_MSC_VER)
#include <corecrt_io.h>
#include <fcntl.h>
#else
#include <io.h>
#include <fcntl.h>
#endif
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

typedef struct {
    XrSourceRepl *owner;
    const char *namespace_id;
    char *buffer;
    size_t buffer_size, buffer_len;
    bool use_color, failed;
    char *history[REPL_MAX_HISTORY];
    size_t history_count;
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
    "var",        "const",    "fn",          "class",        "interface",
    "enum",       "type",     "if",          "else",         "while",
    "for",        "in",       "is",          "break",        "continue",
    "return",     "match",    "true",        "false",        "null",
    "import",     "export",   "from",        "as",           "go",
    "await",      "select",   "defer",       "scope",        "after",
    "try",        "catch",    "throw",       "new",          "this",
    "super",      "extends",  "implements",  "static",       "private",
    "final",      "operator", "void",
    "string",     "bool",     "Array",       "Map",          "Set",
    "JSON.Value", "Channel",  "Array<u8>", "BigInt",       "StringBuilder",
    "PanicInfo",  "dump",     "typeOf",       "typeName",
#define XR_CORE_INTRINSIC(id, stable_id, source_name, category, call_form, parameter_shape,       \
                          min_arity, max_arity, result_shape, effect_kind, flow_rule,               \
                          expected_failure_channel, semantic_op, target_applicability,              \
                          diagnostic_name)                                                          \
    source_name,
#include "../../shared/xr_core_intrinsic.def"
#undef XR_CORE_INTRINSIC
    "copy",        "chr",      "Coro",         "CoroPool",
    NULL,
};

/* Set during repl_run() so the readline generator can reach the
 * current ReplState (and through it, the active owner's published bindings).
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
    static int phase;     /* 0 = dots, 1 = keywords, 2 = exact scalars, 3 = repl symbols */
    static int idx;       /* index within the current phase list */
    static size_t len;    /* cached strlen(text) for prefix match */
    static bool dot_only; /* true when text begins with '.' */

    if (state == 0) {
        phase = 0;
        idx = 0;
        len = strlen(text);
        dot_only = (len > 0 && text[0] == '.');
    }

    /* Dot commands precede language completions. */
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

    /* Language keywords and builtins precede bindings. */
    if (phase == 1) {
        while (k_repl_keywords[idx]) {
            const char *kw = k_repl_keywords[idx++];
            if (strncmp(kw, text, len) == 0)
                return xr_strdup(kw);
        }
        phase = 2;
        idx = 0;
    }

    /* Exact scalar keywords come from their semantic registry. */
    if (phase == 2) {
        size_t scalar_count = 0;
        const XrExactScalarDesc *scalars = xr_exact_scalar_rows(&scalar_count);
        while ((size_t) idx < scalar_count) {
            const char *name = scalars[idx++].source_name;
            if (strncmp(name, text, len) == 0)
                return xr_strdup(name);
        }
        phase = 3;
        idx = 0;
    }

    /* Published display observations borrow the actual Source owner. */
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    if(g_completion_state && g_completion_state->owner &&
        xray_source_repl_bindings(g_completion_state->owner,&rows,&count)==XR_SOURCE_OK) {
        if(phase==3) {
            while((size_t)idx<count) {
                const XrSourceReplBindingInfo *row=&rows[idx++];
                if(!row->implicit_result && strncmp(row->name,text,len)==0)return xr_strdup(row->name);
            }
            phase=4;idx=0;
        }
        if(phase==4) {
            while((size_t)idx<count) {
                const XrSourceReplBindingInfo *row=&rows[idx++];
                if(row->implicit_result && strncmp(row->name,text,len)==0)return xr_strdup(row->name);
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
#ifdef HAS_READLINE
static char *get_history_file(void) {
    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char *path = (char *) xr_malloc(512);
    if(!path)return NULL;
    snprintf(path, 512, "%s/%s", home, REPL_HISTORY_FILE);
    return path;
}
#endif

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
    printf("  .history           Show recent history\n");
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
    printf("  var x = 1              // mutable variable\n");
    printf("  const PI = 3.14        // constant\n");
    printf("  fn add(a: i64, b: i64) -> i64 { return a + b }\n");
    printf("  (x: i64) -> x * 2      // arrow function\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Control Flow:\n");
    printf("  if (x > 0) { } else { }\n");
    printf("  for (var i = 0; i < 10; i++) { }\n");
    printf("  for (item in array) { }\n");
    printf("  while (cond) { }\n");
    printf("  match (x) { 1 -> \"one\", _ -> \"other\" }\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Class:\n");
    printf("  class Point {\n");
    printf("    x = 0; y = 0\n");
    printf("    init(x, y) { this.x = x; this.y = y }\n");
    printf("    distance() { (this.x**2 + this.y**2)**0.5 }\n");
    printf("  }\n");
    printf("\n");
}

// Print types help
static void print_help_types(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Basic Types:\n");
    printf("  i8, i16, i32, i64, u8, u16, u32, u64\n");
    printf("  f32, f64, isize, usize, string, bool, null\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Container Types:\n");
    printf("  Array       [1, 2, 3]\n");
    printf("  Map         #{\"a\": 1, \"b\": 2}\n");
    printf("  Set         #[1, 2, 3]\n");
    printf("  JSON.Value  JSON.value({name: \"xray\", version: 1})\n");
    printf("  Array<u8> Array<u8>(1024, 0)\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Type Annotations:\n");
    printf("  var x: i64 = 1\n");
    printf("  var arr: Array<i64> = [1, 2]\n");
    printf("  fn add(a: i64, b: i64) -> i64 { return a + b }\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Type Conversion:\n");
    printf("  3.14 as i64       // 3\n");
    printf("  f64.parse(\"3.14\") // 3.14\n");
    printf("  string(123)    // \"123\"\n");
    printf("  typeOf(x)      // Type.i64\n");
    printf("  typeName(x)    // \"i64\"\n");
    printf("  typeName<T>()  // static type display name\n");
    printf("\n");
}

// Print coroutine help
static void print_help_coro(ReplState *state) {
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Coroutine Basics:\n");
    printf("  var t = go func()      // spawn coroutine\n");
    printf("  await t                // wait for completion\n");
    printf("  yield                  // yield execution\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Channel:\n");
    printf("  const ch = new Channel<i64>()\n");
    printf("  const ch = new Channel<i64>(10)\n");
    printf("  ch.send(value)         // blocking send\n");
    printf("  match (ch.recv()) { Recv.Value(v) -> print(v); _ -> {} }\n");
    printf("  ch.close()             // close channel\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Select:\n");
    printf("  select {\n");
    printf("    msg from ch1 -> { print(msg) }\n");
    printf("    v to ch2     -> { print(\"sent\") }\n");
    printf("    after 1000   -> { print(\"timeout\") }\n");
    printf("    _            -> { print(\"idle\") }\n");
    printf("  }\n");
    printf("\n");
    print_colored(state, XR_CLR_BOLD, "Shared Variables:\n");
    printf("  const CFG = {...}  // read-only sharing\n");
    printf("  const data = [...]   // deep immutable identity\n");
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

static const char *repl_status_text(XrSourceStatus status) {
    static const char *const names[]={"success","invalid argument","source not found","invalid source",
        "resource limit reached","compiler budget exhausted","out of memory","I/O failure","unsupported operation",
        "source rejected","stopped session","operation busy","execution failed"};
    return (unsigned)status<sizeof(names)/sizeof(*names)?names[status]:"internal failure";
}
static void repl_failure(ReplState *state,XrSourceStatus status,XrSourceDiagnostic *diagnostic) {
    XrSourceDiagnosticInfo info={0};
    if(diagnostic && xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK) {
        if(info.path)fprintf(stderr,"%s:%d:%d: %s\n",info.path,info.line,info.column,info.message);
        else fprintf(stderr,"Error: %s\n",info.message);
    } else xr_cli_error("repl",repl_status_text(status));
    if(status==XR_SOURCE_BUDGET || status==XR_SOURCE_OUT_OF_MEMORY || status==XR_SOURCE_LIMIT ||
        status==XR_SOURCE_IO || status==XR_SOURCE_BAD_STATE || status==XR_SOURCE_EXECUTION_FAILED)state->failed=true;
}
static bool repl_open(ReplState *state,XrSourceRepl **output) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    XrSourceStatus status=xray_source_compiler_options_init(&compiler,sizeof(compiler));
    if(status==XR_SOURCE_OK)status=xray_source_call_options_init(&runtime,sizeof(runtime));
    if(status==XR_SOURCE_OK)status=xray_source_repl_new(&compiler,state->namespace_id,&runtime,output);
    if(status!=XR_SOURCE_OK)repl_failure(state,status,NULL);
    return status==XR_SOURCE_OK;
}
static bool repl_close(ReplState *state,XrSourceRepl **owner) {
    XrSourceEvent cleanup={0};XrSourceStatus status=xray_source_repl_drop(owner,&cleanup);
    if(status!=XR_SOURCE_OK || cleanup.state!=XR_SOURCE_RUN_READY) {
        repl_failure(state,status==XR_SOURCE_OK?XR_SOURCE_EXECUTION_FAILED:status,NULL);return false;
    }
    return true;
}
static bool is_input_complete(ReplState *state) {
    XrSourceInputState complete=XR_SOURCE_INPUT_COMPLETE;
    XrSourceStatus status=xray_source_repl_check_input(state->owner,state->buffer,state->buffer_len,&complete);
    if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);state->failed=true;return false;}
    return complete==XR_SOURCE_INPUT_COMPLETE;
}
static void append_to_buffer(ReplState *state,const char *line) {
    size_t length=strlen(line);
    if(length>SIZE_MAX-state->buffer_len-2){state->failed=true;return;}
    size_t needed=state->buffer_len+length+2;
    if(needed>state->buffer_size) {
        size_t capacity=state->buffer_size;
        while(capacity<needed && capacity<=SIZE_MAX/2)capacity*=2;
        if(capacity<needed){state->failed=true;return;}
        char *grown=xr_realloc(state->buffer,capacity);
        if(!grown){repl_failure(state,XR_SOURCE_OUT_OF_MEMORY,NULL);return;}
        state->buffer=grown;state->buffer_size=capacity;
    }
    if(state->buffer_len)state->buffer[state->buffer_len++]='\n';
    memcpy(state->buffer+state->buffer_len,line,length);state->buffer_len+=length;state->buffer[state->buffer_len]=0;
}
static void reset_buffer(ReplState *state) {state->buffer_len=0;state->buffer[0]=0;}
static void repl_history_add(ReplState *state) {
    char *copy=xr_strdup(state->buffer);if(!copy){repl_failure(state,XR_SOURCE_OUT_OF_MEMORY,NULL);return;}
    if(state->history_count==REPL_MAX_HISTORY) {
        xr_free(state->history[0]);memmove(state->history,state->history+1,(REPL_MAX_HISTORY-1)*sizeof(*state->history));
        --state->history_count;
    }
    state->history[state->history_count++]=copy;
#ifdef HAS_READLINE
    add_history(state->buffer);
#endif
}
static bool repl_wait(ReplState *state,const XrSourceEvent *event) {
    XrSourceWait request={0};XrSourceStatus status=xray_source_repl_wait_request(state->owner,event->epoch,event->wake,&request,sizeof(request));
    if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);return false;}
    if(request.kind==XR_SOURCE_WAIT_TASK){repl_failure(state,XR_SOURCE_UNSUPPORTED,NULL);state->failed=true;return false;}
    XrXirWaitRequest timer={0};timer.kind=request.kind==XR_SOURCE_WAIT_TIMER?XR_XIR_WAIT_TIMER_MS:XR_XIR_WAIT_YIELD;
    timer.after_ms=request.after_ms;
    XrXirHostWait wait={0};XrXirHostWaitStatus waiting=xr_xir_host_wait_begin(&wait,&timer);
    while(waiting==XR_XIR_HOST_WAIT_PENDING && !g_repl_interrupted)
        waiting=xr_xir_host_wait_poll(&wait,UINT64_C(10000000));
    if(g_repl_interrupted)return true;
    if(waiting!=XR_XIR_HOST_WAIT_DUE){repl_failure(state,XR_SOURCE_IO,NULL);return false;}
    status=xray_source_repl_resume(state->owner,event->epoch,event->wake);
    if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);return false;}
    return true;
}
static void repl_drive(ReplState *state) {
    XrSourceEvent event={0};XrSourceStatus status=XR_SOURCE_OK;
    for(;;) {
        if(g_repl_interrupted) {
            g_repl_interrupted=0;status=xray_source_repl_cancel(state->owner);
            if(status!=XR_SOURCE_OK)break;
        }
        status=xray_source_repl_poll(state->owner,256,&event);if(status!=XR_SOURCE_OK)break;
        if(event.state==XR_SOURCE_RUN_READY)continue;
        if(event.state==XR_SOURCE_RUN_SUSPENDED){if(!repl_wait(state,&event))return;continue;}
        break;
    }
    if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);state->failed=true;return;}
    XrSourceResult *result=NULL;status=xray_source_repl_take(state->owner,&result);
    if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);state->failed=true;return;}
    if(event.state==XR_SOURCE_RUN_CANCELLED)fputs("Interrupted\n",stderr);
    else if(event.state!=XR_SOURCE_RUN_RETURNED) {
        if(xray_source_result_format(result,NULL,NULL)!=XR_SOURCE_OK)repl_failure(state,XR_SOURCE_EXECUTION_FAILED,NULL);
        if(event.state!=XR_SOURCE_RUN_PANICKED && event.state!=XR_SOURCE_RUN_THROWN)state->failed=true;
    }
    xray_source_result_free(result);
}
static void execute_code(ReplState *state,const char *code,bool query) {
    const XrSourceText input={NULL,state->namespace_id,code,strlen(code)};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=query?xray_source_repl_begin_type(state->owner,&input,&diagnostic):
        xray_source_repl_begin(state->owner,&input,&diagnostic);
    if(status==XR_SOURCE_OK)repl_drive(state);else repl_failure(state,status,diagnostic);
    xray_source_diagnostic_free(diagnostic);
}
static void cmd_load(ReplState *state,const char *filename) {
    while(filename && isspace((unsigned char)*filename))++filename;
    if(!filename || !*filename){fputs("Error: please specify filename\nUsage: .load <filename>\n",stderr);return;}
    printf("Loading %s...\n",filename);XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_repl_begin_file(state->owner,filename,&diagnostic);
    if(status==XR_SOURCE_OK)repl_drive(state);else repl_failure(state,status,diagnostic);
    xray_source_diagnostic_free(diagnostic);
}
typedef struct ReplVarsBuffer {char bytes[4096];size_t used;bool truncated;} ReplVarsBuffer;
static XrSourceWriteStatus repl_vars_write(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    ReplVarsBuffer *buffer=context;(void)stream;
    size_t available=sizeof(buffer->bytes)-buffer->used,copy=length<available?length:available;
    memcpy(buffer->bytes+buffer->used,bytes,copy);buffer->used+=copy;if(length>copy)buffer->truncated=true;
    return XR_SOURCE_WRITE_OK;
}
static void repl_vars(ReplState *state) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    XrSourceStatus status=xray_source_repl_bindings(state->owner,&rows,&count);
    if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);return;}
    if(!count){puts("  (no bindings)");return;}
    for(size_t i=0;i<count;++i) {
        const XrSourceReplBindingInfo *row=&rows[i];ReplVarsBuffer buffer={0};XrSourceValue *value=NULL;
        if(row->kind==XR_SOURCE_REPL_VALUE || row->kind==XR_SOURCE_REPL_FUNCTION) {
            status=xray_source_repl_read(state->owner,row->name,&value);
            if(status==XR_SOURCE_OK)status=xray_source_value_format(value,repl_vars_write,&buffer);
            xray_source_value_free(value);
            if(status!=XR_SOURCE_OK){repl_failure(state,status,NULL);return;}
        } else {
            const char *display=row->kind==XR_SOURCE_REPL_TYPE?"<type>":row->kind==XR_SOURCE_REPL_MODULE?"<module>":"<import>";
            (void)repl_vars_write(&buffer,XR_SOURCE_STDOUT,display,strlen(display));
        }
        const char *kind=row->kind==XR_SOURCE_REPL_VALUE?(row->is_const?"const":"var  "):
            row->kind==XR_SOURCE_REPL_FUNCTION?"fn   ":row->kind==XR_SOURCE_REPL_TYPE?"type ":
            row->kind==XR_SOURCE_REPL_MODULE?"mod  ":"import";
        printf("  %s %s : %s =",kind,row->name,row->type_name?row->type_name:"<unavailable>");
        fputs(buffer.used<=72 && !buffer.truncated?" ":"\n        ",stdout);
        if(buffer.used && fwrite(buffer.bytes,1,buffer.used,stdout)!=buffer.used){repl_failure(state,XR_SOURCE_IO,NULL);return;}
        puts(buffer.truncated?" ..":"");
    }
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
    execute_code(state, expr, false);
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
        XrSourceRepl *replacement=NULL;
        if(!repl_open(state,&replacement))return true;
        if(!repl_close(state,&state->owner)){(void)repl_close(state,&replacement);return true;}
        state->owner=replacement;
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
        repl_vars(state);
        return true;
    }

    // .type <expr> — show runtime type of an expression
    if (strncmp(input, ".type", 5) == 0 && (input[5] == '\0' || input[5] == ' ')) {
        const char *expression=input+5;while(isspace((unsigned char)*expression))++expression;
        if(!*expression)puts("Usage: .type <expression>");else execute_code(state,expression,true);
        return true;
    }

    if(strcmp(input,".history")==0) {
#ifdef HAS_READLINE
        int start=history_length>20?history_length-19:1;
        for(int i=start;i<=history_length;++i){HIST_ENTRY *entry=history_get(i);if(entry)printf("%4d  %s\n",i,entry->line);}
#else
        size_t start=state->history_count>20?state->history_count-20:0;
        for(size_t i=start;i<state->history_count;++i)printf("%4zu  %s\n",i+1,state->history[i]);
#endif
        return true;
    }

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

    size_t length=0,capacity=REPL_BUFFER_SIZE;char *line=xr_malloc(capacity);
    if(!line){repl_failure(state,XR_SOURCE_OUT_OF_MEMORY,NULL);return NULL;}
    int byte=0;
    while(!g_repl_interrupted && (byte=fgetc(stdin))!=EOF && byte!='\n') {
        if(byte==0){xr_cli_error("repl","input contains a zero byte");state->failed=true;xr_free(line);return NULL;}
        if(length==capacity-1) {
            if(capacity>SIZE_MAX/2){repl_failure(state,XR_SOURCE_LIMIT,NULL);xr_free(line);return NULL;}
            char *grown=xr_realloc(line,capacity*2);
            if(!grown){repl_failure(state,XR_SOURCE_OUT_OF_MEMORY,NULL);xr_free(line);return NULL;}
            line=grown;capacity*=2;
        }
        line[length++]=(char)byte;
    }
    if(ferror(stdin) && !g_repl_interrupted){repl_failure(state,XR_SOURCE_IO,NULL);xr_free(line);return NULL;}
    if(byte==EOF && !length){xr_free(line);return NULL;}
    line[length]=0;return line;
#endif
}

XR_FUNC int cmd_repl(const XrCliInvocation *inv) {
    if(!inv)return XR_CLI_EXIT_INTERNAL;

    ReplState state = {0};
    state.namespace_id=xr_cli_opt_string(&inv->options,"module-id",NULL);
    if(!state.namespace_id || !*state.namespace_id) {
        xr_cli_error("repl","--module-id must be an explicit valid memory-module identity");return XR_CLI_EXIT_USAGE;
    }
    state.use_color=inv->ctx && inv->ctx->color && terminal_supports_color();
#if XR_OS_WINDOWS
    if(_setmode(_fileno(stdout),_O_BINARY)==-1 || _setmode(_fileno(stderr),_O_BINARY)==-1)return XR_CLI_EXIT_INTERNAL;
#endif
    if(!repl_open(&state,&state.owner))return XR_CLI_EXIT_FAIL;

    /* Initialize buffer */
    state.buffer_size = REPL_BUFFER_SIZE;
    state.buffer=xr_malloc(state.buffer_size);
    if(!state.buffer){(void)repl_close(&state,&state.owner);return XR_CLI_EXIT_INTERNAL;}
    state.buffer[0] = '\0';
    state.buffer_len = 0;

    // Setup SIGINT handler
    g_repl_interrupted=0;
    void (*previous_sigint)(int)=signal(SIGINT,repl_sigint_handler);

    // Load history
#ifdef HAS_READLINE
    stifle_history(REPL_MAX_HISTORY);
    char *history_file = get_history_file();
    if (history_file) {
        read_history(history_file);
    }
    /* Wire tab completion.  Set the generator and make `g_completion_state`
     * point at the live ReplState so completion can see live
     * REPL bindings.  Cleared after the loop exits below. */
    g_completion_state = &state;
    rl_attempted_completion_function = repl_completion;
    /* Single-char word break set: anything that ends an identifier.
     * Notably keeps `.` so dot commands complete as a unit, but allows
     * space, parens, operators to bound a word. */
    rl_basic_word_break_characters = (char *) " \t\n\"\\'`@$><=;|&{(";
#endif

    // Print welcome
    print_welcome(&state);

    // REPL main loop
    while (!state.failed) {
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
            /* Record the whole buffer (possibly multi-line) as one
             * history entry.  Empty buffers are skipped so users do
             * not see blank entries when they press Enter on an
             * empty prompt. */
            if (state.buffer_len > 0) {
                repl_history_add(&state);
            }

            if(!state.failed)execute_code(&state, state.buffer, false);
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
    if(previous_sigint!=SIG_ERR)signal(SIGINT,previous_sigint);

    // Cleanup
    if (state.use_color)
        printf("%sbye%s\n", XR_CLR_DIM, XR_CLR_RESET);
    else
        printf("bye\n");
    (void)repl_close(&state,&state.owner);
    for(size_t i=0;i<state.history_count;++i)xr_free(state.history[i]);
    xr_free(state.buffer);

    return state.failed?XR_CLI_EXIT_FAIL:XR_CLI_EXIT_OK;
}
