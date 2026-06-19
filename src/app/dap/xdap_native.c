/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_native.c - Native AOT-binary debugging bridge (DAP -> lldb-dap)
 *
 * See xdap_native.h for the design rationale. This translation unit is a
 * thin, transforming DAP proxy: editor <-> xray <-> lldb-dap. The only
 * message it rewrites is `launch` (compile a `.xr` target to a temporary
 * `-g` native binary, then point lldb-dap at it); everything else is copied
 * through verbatim so DAP sequence numbers stay consistent.
 */

#include "xdap_native.h"

#include "../../base/xdefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XR_OS_WINDOWS)

/* lldb-dap based native debugging is POSIX-only for now. Windows native
 * debugging would route through a different backend (e.g. the VS debug
 * engine) and is intentionally out of scope here. */
XR_FUNC int xdap_native_run(int in_fd, int out_fd, const char *self_exe,
                            const char *debugger_override) {
    (void) in_fd;
    (void) self_exe;
    (void) debugger_override;
    static const char body[] =
        "{\"type\":\"event\",\"seq\":1,\"event\":\"output\",\"body\":{\"category\":\"stderr\","
        "\"output\":\"native DAP backend is not supported on this platform\\n\"}}";
    char header[64];
    int hn = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", sizeof(body) - 1);
    if (out_fd >= 0 && hn > 0) {
        FILE *out = fdopen(out_fd, "w");
        if (out) {
            fwrite(header, 1, (size_t) hn, out);
            fwrite(body, 1, sizeof(body) - 1, out);
            fflush(out);
        }
    }
    return 1;
}

#else  // POSIX

#include "../../base/xmalloc.h"
#include "../../base/xjson.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// ============================================================================
// Bridge state
// ============================================================================

typedef struct {
    int child_in;          // write end -> lldb-dap stdin
    int out_fd;            // write end -> editor
    const char *self_exe;  // path to xray, for compiling .xr targets
    int seq;               // sequence counter for bridge-originated messages

    // Temp artifacts to remove on shutdown (compiled binaries + .dSYM).
    char **tmp_paths;
    int tmp_count;
    int tmp_cap;
} XdapNativeBridge;

// ============================================================================
// Low-level IO helpers
// ============================================================================

static bool write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += (size_t) n;
    }
    return true;
}

// Frame a DAP message body with the Content-Length header and write it.
static bool frame_and_write(int fd, const char *body, size_t body_len) {
    char header[64];
    int hn = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", body_len);
    if (hn <= 0)
        return false;
    if (!write_all(fd, header, (size_t) hn))
        return false;
    return write_all(fd, body, body_len);
}

// ============================================================================
// Debugger discovery
// ============================================================================

// Search PATH for an executable `name`. Returns an owned absolute path or NULL.
static char *which_in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !path[0])
        return NULL;
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t dirlen = colon ? (size_t) (colon - p) : strlen(p);
        if (dirlen > 0 && dirlen < 4000) {
            char candidate[4096];
            snprintf(candidate, sizeof(candidate), "%.*s/%s", (int) dirlen, p, name);
            if (access(candidate, X_OK) == 0)
                return xr_strdup(candidate);
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return NULL;
}

// Locate lldb-dap. Order: explicit override, XRAY_LLDB_DAP, well-known
// toolchain locations, then PATH. Returns an owned path or NULL if not found.
static char *find_debugger(const char *override) {
    static const char *well_known[] = {
        "/opt/homebrew/opt/llvm/bin/lldb-dap",
        "/usr/local/opt/llvm/bin/lldb-dap",
        "/usr/bin/lldb-dap",
        "/usr/local/bin/lldb-dap",
        "/Library/Developer/CommandLineTools/usr/bin/lldb-dap",
    };
    if (override && override[0] && access(override, X_OK) == 0)
        return xr_strdup(override);
    const char *env = getenv("XRAY_LLDB_DAP");
    if (env && env[0] && access(env, X_OK) == 0)
        return xr_strdup(env);
    for (size_t i = 0; i < sizeof(well_known) / sizeof(well_known[0]); i++) {
        if (access(well_known[i], X_OK) == 0)
            return xr_strdup(well_known[i]);
    }
    return which_in_path("lldb-dap");
}

// ============================================================================
// Child process spawning (stdin/stdout connected to pipes)
// ============================================================================

// Spawn `prog` with its stdin/stdout connected to pipes. On success returns
// the child pid and sets *in_w (write to child stdin) and *out_r (read from
// child stdout). The child keeps the parent's stderr for diagnostics.
static pid_t spawn_piped(const char *prog, char *const argv[], int *in_w, int *out_r) {
    int to_child[2];
    int from_child[2];
    if (pipe(to_child) != 0)
        return -1;
    if (pipe(from_child) != 0) {
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    }
    if (pid == 0) {
        // Child: wire stdio, exec.
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execvp(prog, argv);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    *in_w = to_child[1];
    *out_r = from_child[0];
    return pid;
}

// ============================================================================
// .xr -> native -g compilation (reuses `xray build --native -g`)
// ============================================================================

// Run `<self_exe> build --native -g -o out_bin src`, sending the child's
// stdout/stderr to log_path so it never corrupts the DAP stream on out_fd.
// Returns true on a clean (exit 0) compile.
static bool run_compile(const char *self_exe, const char *src, const char *out_bin,
                        const char *log_path) {
    const char *prog = (self_exe && self_exe[0]) ? self_exe : "xray";
    const char *argv[] = {prog, "build", "--native", "-g", "-o", out_bin, src, NULL};

    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }
        execvp(prog, (char *const *) argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Read a whole (small) file into an owned NUL-terminated buffer, or NULL.
static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (len + n + 1 > cap) {
            size_t ncap = (cap < 8192) ? 8192 : cap * 2;
            while (ncap < len + n + 1)
                ncap *= 2;
            char *nb = (char *) xr_realloc(buf, ncap);
            if (!nb) {
                xr_free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    fclose(f);
    if (!buf) {
        buf = (char *) xr_malloc(1);
        if (!buf)
            return NULL;
    }
    buf[len] = '\0';
    return buf;
}

// ============================================================================
// Temp artifact tracking
// ============================================================================

static void bridge_track_tmp(XdapNativeBridge *br, const char *path) {
    if (br->tmp_count >= br->tmp_cap) {
        int ncap = br->tmp_cap < 4 ? 4 : br->tmp_cap * 2;
        char **np = (char **) xr_realloc(br->tmp_paths, (size_t) ncap * sizeof(char *));
        if (!np)
            return;
        br->tmp_paths = np;
        br->tmp_cap = ncap;
    }
    br->tmp_paths[br->tmp_count++] = xr_strdup(path);
}

static void rm_rf(const char *path);

static void bridge_cleanup_tmp(XdapNativeBridge *br) {
    for (int i = 0; i < br->tmp_count; i++) {
        if (!br->tmp_paths[i])
            continue;
        unlink(br->tmp_paths[i]);
        char dsym[4096];
        snprintf(dsym, sizeof(dsym), "%s.dSYM", br->tmp_paths[i]);
        rm_rf(dsym);
        xr_free(br->tmp_paths[i]);
    }
    xr_free(br->tmp_paths);
    br->tmp_paths = NULL;
    br->tmp_count = 0;
    br->tmp_cap = 0;
}

// Best-effort recursive remove (used only for the small .dSYM bundle).
static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        // The .dSYM bundle is a small directory tree; walk and remove it
        // entry by entry rather than shelling out.
        DIR *d = opendir(path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                char child[4096];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

// ============================================================================
// Bridge-originated DAP messages (errors on the launch path)
// ============================================================================

static void bridge_send_output(XdapNativeBridge *br, const char *category, const char *text) {
    XrJsonValue *msg = xjson_new_object();
    XrJsonValue *body = xjson_new_object();
    XJSON_SET_STRING(msg, "type", "event");
    XJSON_SET_INT(msg, "seq", br->seq++);
    XJSON_SET_STRING(msg, "event", "output");
    XJSON_SET_STRING(body, "category", category);
    XJSON_SET_STRING(body, "output", text);
    XJSON_SET(msg, "body", body);
    size_t len = 0;
    char *s = xjson_stringify(msg, &len);
    if (s) {
        frame_and_write(br->out_fd, s, len);
        xr_free(s);
    }
    xjson_free(msg);
}

static void bridge_send_launch_error(XdapNativeBridge *br, int request_seq, const char *message) {
    XrJsonValue *msg = xjson_new_object();
    XJSON_SET_STRING(msg, "type", "response");
    XJSON_SET_INT(msg, "seq", br->seq++);
    XJSON_SET_INT(msg, "request_seq", request_seq);
    XJSON_SET_STRING(msg, "command", "launch");
    XJSON_SET_BOOL(msg, "success", false);
    XJSON_SET_STRING(msg, "message", message);
    size_t len = 0;
    char *s = xjson_stringify(msg, &len);
    if (s) {
        frame_and_write(br->out_fd, s, len);
        xr_free(s);
    }
    xjson_free(msg);
}

// ============================================================================
// Launch handling
// ============================================================================

typedef enum {
    LAUNCH_FORWARD_ORIGINAL,   // not a .xr target: pass the original bytes through
    LAUNCH_FORWARD_REWRITTEN,  // compiled: a rewritten body is in *out_body
    LAUNCH_HANDLED_LOCALLY,    // compile failed: error already sent to editor
} LaunchResult;

// Inspect a parsed launch request. If `arguments.program` is a `.xr` file,
// compile it to a temp `-g` native binary and rewrite program in-place.
static LaunchResult handle_launch(XdapNativeBridge *br, XrJsonValue *msg, char **out_body,
                                  size_t *out_len) {
    XrJsonValue *args = xjson_get(msg, "arguments");
    if (!args || !xjson_is_object(args))
        return LAUNCH_FORWARD_ORIGINAL;
    const char *program = xjson_get_string(args, "program");
    if (!program)
        return LAUNCH_FORWARD_ORIGINAL;
    size_t plen = strlen(program);
    if (plen < 3 || strcmp(program + plen - 3, ".xr") != 0)
        return LAUNCH_FORWARD_ORIGINAL;  // already a native binary

    int request_seq = (int) xjson_get_int(msg, "seq");

    char out_bin[4096];
    char log_path[4096];
    long pid = (long) getpid();
    snprintf(out_bin, sizeof(out_bin), "/tmp/xray_dap_native_%ld_%d.bin", pid, br->seq);
    snprintf(log_path, sizeof(log_path), "/tmp/xray_dap_native_%ld_%d.log", pid, br->seq);

    if (!run_compile(br->self_exe, program, out_bin, log_path)) {
        char *log = read_text_file(log_path);
        bridge_send_output(br, "stderr", "xray: native debug build failed\n");
        if (log && log[0])
            bridge_send_output(br, "stderr", log);
        bridge_send_launch_error(br, request_seq, "native debug build failed");
        xr_free(log);
        unlink(log_path);
        return LAUNCH_HANDLED_LOCALLY;
    }
    unlink(log_path);
    bridge_track_tmp(br, out_bin);

    xjson_object_set(args, "program", xjson_new_string(out_bin));
    char *s = xjson_stringify(msg, out_len);
    if (!s)
        return LAUNCH_FORWARD_ORIGINAL;
    *out_body = s;
    return LAUNCH_FORWARD_REWRITTEN;
}

// Canonicalize arguments.source.path via realpath so the breakpoint file
// matches the realpath-based path xray records in DWARF (#line). Editors send
// the path the user opened, which may differ from the canonical path by a
// symlink (e.g. macOS /tmp -> /private/tmp, or a symlinked project root);
// without this, lldb-dap silently fails to bind the breakpoint. Returns true
// and fills *out_body/*out_len when a rewrite was applied.
static bool transform_set_breakpoints(XrJsonValue *msg, char **out_body, size_t *out_len) {
    XrJsonValue *args = xjson_get(msg, "arguments");
    if (!args || !xjson_is_object(args))
        return false;
    XrJsonValue *source = xjson_get(args, "source");
    if (!source || !xjson_is_object(source))
        return false;
    const char *path = xjson_get_string(source, "path");
    if (!path || !path[0])
        return false;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return false;
    if (strcmp(resolved, path) == 0)
        return false;  // already canonical
    xjson_object_set(source, "path", xjson_new_string(resolved));
    char *s = xjson_stringify(msg, out_len);
    if (!s)
        return false;
    *out_body = s;
    return true;
}

// Forward either a rewritten body or the original frame to lldb-dap.
static bool forward_rewritten_or_original(XdapNativeBridge *br, bool rewritten, char *body,
                                          size_t body_len, const char *frame, size_t frame_len) {
    if (rewritten) {
        bool ok = frame_and_write(br->child_in, body, body_len);
        xr_free(body);
        return ok;
    }
    return write_all(br->child_in, frame, frame_len);
}

// Process one complete DAP message from the editor. Returns false on a fatal
// write error (session should end).
static bool process_client_message(XdapNativeBridge *br, const char *frame, size_t frame_len,
                                   const char *body, size_t body_len) {
    XrJsonValue *msg = xjson_parse(body, body_len);
    const char *command = msg ? xjson_get_string(msg, "command") : NULL;

    if (msg && command && strcmp(command, "launch") == 0) {
        char *rewritten = NULL;
        size_t rewritten_len = 0;
        LaunchResult r = handle_launch(br, msg, &rewritten, &rewritten_len);
        bool ok = true;
        if (r == LAUNCH_FORWARD_REWRITTEN) {
            ok = frame_and_write(br->child_in, rewritten, rewritten_len);
            xr_free(rewritten);
        } else if (r == LAUNCH_FORWARD_ORIGINAL) {
            ok = write_all(br->child_in, frame, frame_len);
        }
        // LAUNCH_HANDLED_LOCALLY: nothing forwarded; editor got the error.
        xjson_free(msg);
        return ok;
    }

    if (msg && command && strcmp(command, "setBreakpoints") == 0) {
        char *rewritten = NULL;
        size_t rewritten_len = 0;
        bool changed = transform_set_breakpoints(msg, &rewritten, &rewritten_len);
        bool ok =
            forward_rewritten_or_original(br, changed, rewritten, rewritten_len, frame, frame_len);
        xjson_free(msg);
        return ok;
    }

    xjson_free(msg);
    // Everything else: forward verbatim to keep seqs consistent.
    return write_all(br->child_in, frame, frame_len);
}

// ============================================================================
// Frame extraction from the editor stream
// ============================================================================

// Parse the Content-Length of a header that ends at `header_end` (the index
// just past the terminating \r\n\r\n). Returns -1 if absent/invalid.
static long parse_content_length(const char *buf, size_t header_end) {
    const char *key = "content-length:";
    size_t keylen = strlen(key);
    for (size_t i = 0; i + keylen <= header_end; i++) {
        bool match = true;
        for (size_t j = 0; j < keylen; j++) {
            char c = buf[i + j];
            if (c >= 'A' && c <= 'Z')
                c = (char) (c - 'A' + 'a');
            if (c != key[j]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        size_t p = i + keylen;
        while (p < header_end && (buf[p] == ' ' || buf[p] == '\t'))
            p++;
        long val = 0;
        bool any = false;
        while (p < header_end && buf[p] >= '0' && buf[p] <= '9') {
            val = val * 10 + (buf[p] - '0');
            any = true;
            p++;
        }
        return any ? val : -1;
    }
    return -1;
}

// ============================================================================
// Main proxy loop
// ============================================================================

XR_FUNC int xdap_native_run(int in_fd, int out_fd, const char *self_exe,
                            const char *debugger_override) {
    signal(SIGPIPE, SIG_IGN);

    char *debugger = find_debugger(debugger_override);
    if (!debugger) {
        XdapNativeBridge tmp = {0};
        tmp.out_fd = out_fd;
        tmp.seq = 1;
        bridge_send_output(&tmp, "stderr",
                           "xray: native debugging requires 'lldb-dap' (set XRAY_LLDB_DAP or "
                           "pass --debugger)\n");
        return 1;
    }

    int child_in = -1;
    int child_out = -1;
    char *const dbg_argv[] = {debugger, NULL};
    pid_t child = spawn_piped(debugger, dbg_argv, &child_in, &child_out);
    if (child < 0) {
        XdapNativeBridge tmp = {0};
        tmp.out_fd = out_fd;
        tmp.seq = 1;
        bridge_send_output(&tmp, "stderr", "xray: failed to start lldb-dap\n");
        xr_free(debugger);
        return 1;
    }

    XdapNativeBridge br = {0};
    br.child_in = child_in;
    br.out_fd = out_fd;
    br.self_exe = self_exe;
    br.seq = 1;

    // Editor-side accumulation buffer for frame extraction.
    char *cbuf = NULL;
    size_t clen = 0;
    size_t ccap = 0;

    bool running = true;
    int exit_code = 0;
    char rdbuf[8192];

    while (running) {
        struct pollfd fds[2];
        fds[0].fd = in_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = child_out;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int pr = poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // lldb-dap -> editor: raw passthrough.
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(child_out, rdbuf, sizeof(rdbuf));
            if (n > 0) {
                if (!write_all(out_fd, rdbuf, (size_t) n))
                    break;
            } else if (n == 0) {
                break;  // lldb-dap exited
            } else if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }

        // editor -> lldb-dap: accumulate, extract frames, transform launch.
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(in_fd, rdbuf, sizeof(rdbuf));
            if (n > 0) {
                if (clen + (size_t) n + 1 > ccap) {
                    size_t ncap = ccap < 8192 ? 16384 : ccap * 2;
                    while (ncap < clen + (size_t) n + 1)
                        ncap *= 2;
                    char *nb = (char *) xr_realloc(cbuf, ncap);
                    if (!nb)
                        break;
                    cbuf = nb;
                    ccap = ncap;
                }
                memcpy(cbuf + clen, rdbuf, (size_t) n);
                clen += (size_t) n;

                // Extract as many complete frames as are buffered.
                for (;;) {
                    // Find header terminator \r\n\r\n.
                    size_t header_end = 0;
                    bool found = false;
                    if (clen >= 4) {
                        for (size_t i = 0; i + 4 <= clen; i++) {
                            if (cbuf[i] == '\r' && cbuf[i + 1] == '\n' && cbuf[i + 2] == '\r' &&
                                cbuf[i + 3] == '\n') {
                                header_end = i + 4;
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found)
                        break;
                    long content_length = parse_content_length(cbuf, header_end);
                    if (content_length < 0) {
                        // Malformed header: drop accumulated bytes to resync.
                        clen = 0;
                        break;
                    }
                    size_t total = header_end + (size_t) content_length;
                    if (clen < total)
                        break;  // wait for the rest of the body

                    if (!process_client_message(&br, cbuf, total, cbuf + header_end,
                                                (size_t) content_length)) {
                        running = false;
                        break;
                    }
                    memmove(cbuf, cbuf + total, clen - total);
                    clen -= total;
                }
            } else if (n == 0) {
                break;  // editor closed
            } else if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
    }

    // Shutdown: closing child stdin asks lldb-dap to exit, then reap it.
    if (child_in >= 0)
        close(child_in);
    if (child_out >= 0)
        close(child_out);
    if (child > 0) {
        int status = 0;
        for (int i = 0; i < 50; i++) {
            pid_t w = waitpid(child, &status, WNOHANG);
            if (w == child || (w < 0 && errno != EINTR))
                break;
            usleep(20000);  // 20ms, up to ~1s total
            if (i == 49)
                kill(child, SIGTERM);
        }
        waitpid(child, &status, 0);
    }

    xr_free(cbuf);
    bridge_cleanup_tmp(&br);
    xr_free(debugger);
    return exit_code;
}

#endif  // platform
