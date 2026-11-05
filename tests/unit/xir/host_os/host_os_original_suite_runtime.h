/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_original_suite_runtime.h - Execute original per-file test catalogs
 */
#ifndef HOST_OS_ORIGINAL_SUITE_RUNTIME_H
#define HOST_OS_ORIGINAL_SUITE_RUNTIME_H
#include "program/xr_xir_source_product.h"
#include "execution/xr_xir_host_cli.h"
#include "execution/xr_xir_host_time.h"
#include "base/xsha256.h"
#include "os/os_time.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <fcntl.h>
#include <io.h>
#endif
#define HOST_ORIGINAL_FILE_COUNT 12u
#define HOST_ORIGINAL_FILE_NS UINT64_C(120000000000)
#define HOST_ORIGINAL_WAIT_NS UINT64_C(10000000)
/* Wire rows are fixture-private C constants, not execution authority. */
typedef struct HostOriginalWireRow {
    uint32_t function, role, timeout_seconds;
    const char *name;
} HostOriginalWireRow;
typedef struct HostOriginalInput {
    const char *path, *prefix, *canonical_sha256;
    const char *const *names;
    uint32_t expected;
} HostOriginalInput;
static const char *const host_original_names_0[] = {"test_platform","test_arch","test_sep","test_eol","test_getenv_path","test_getenv_nonexistent","test_setenv_getenv","test_setenv_overwrite","test_environ","test_getpid","test_getcwd","test_hostname","test_tmpdir","test_tmpdir_skips_empty_env_candidate","test_chdir"};
static const char *const host_original_names_1[] = {"test_username","test_homedir","test_homedir_skips_empty_home_env","test_uid","test_gid","test_cpuCount","test_totalMemory","test_freeMemory","test_uptime","test_loadavg","test_ppid","test_clock","test_exec_echo","test_exec_stderr","test_exec_exit_code","test_exec_command_output","test_exec_byte_capture_limits","test_setenv_return_value","test_unsetenv_return_value","test_chdir_invalid_path"};
static const char *const host_original_names_2[] = {"test_temp_file","test_write_read_file","test_read_lines","test_append_file","test_file_stat","test_is_file","test_file_size","test_mkdir_and_isdir","test_mkdirp","test_read_dir","test_copy_file","test_rename","test_touch","test_cwd","test_realpath","test_remove_all"};
static const char *const host_original_names_3[] = {"test_read_nonexistent","test_exists_nonexistent","test_is_file_nonexistent","test_is_dir_nonexistent","test_write_empty","test_overwrite_file","test_large_write_read","test_read_dir_recursive","test_mkdirp_existing_and_trailing_slashes","test_remove_nonexistent","test_read_lines_empty","test_read_lines_single","test_read_lines_trailing_newline","test_read_lines_middle_empty_and_crlf"};
static const char *const host_original_names_4[] = {"test_basic_file_ops","test_file_info","test_directory_ops","test_copy_and_move","test_touch_and_chmod","test_realpath","test_chdir"};
static const char *const host_original_names_5[] = {"test_read_file_rejects_invalid_utf8","test_read_file_bytes_still_returns_the_raw_bytes","test_valid_utf8_is_unaffected"};
static const char *const host_original_names_6[] = {"test_lists_every_level_relative_to_the_root","test_a_symlinked_directory_is_listed_but_not_entered","test_a_path_that_is_not_a_directory_answers_null"};
static const char *const host_original_names_7[] = {"test_empty_path_is_rejected","test_creates_every_missing_level","test_existing_directory_succeeds","test_trailing_and_repeated_separators","test_root_path_succeeds","test_backslash_is_a_filename_byte_not_a_separator","test_fails_when_an_intermediate_level_is_a_file"};
static const char *const host_original_names_8[] = {"test_removes_a_whole_tree","test_removes_a_single_file","test_missing_path_is_reported_as_failure","test_a_symlink_is_removed_without_following_it","test_a_tree_deeper_than_the_ceiling_is_refused_whole"};
static const char *const host_original_names_9[] = {"test_creates_a_missing_file_empty","test_does_not_truncate_an_existing_file","test_succeeds_on_a_directory","test_fails_when_the_parent_is_missing"};
static const char *const host_original_names_10[] = {"test_temp_dir_is_created_and_is_a_directory","test_temp_file_is_created_and_is_a_file","test_a_trailing_separator_in_the_root_is_not_doubled","test_the_root_comes_from_the_environment","test_two_calls_do_not_collide"};
static const char *const host_original_names_11[] = {"test_read_file_rejects_invalid_utf8","test_read_file_bytes_still_returns_the_raw_bytes","test_valid_utf8_is_unaffected"};
static const HostOriginalInput host_original_inputs[] = {
    {"tests/regression/10_stdlib/1160_os_basic.xr","host_original_os_1160","79246554e3eae216a85e808e55e0d79e79d9c962a8c5907b413f431ea79ba859",host_original_names_0,15u},
    {"tests/regression/10_stdlib/1161_os_extended.xr","host_original_os_1161","77a61124d5d0e1017973c79cc1a022844185a295d86195cfc22136455a84ffe2",host_original_names_1,20u},
    {"tests/regression/10_stdlib/1190_io_basic.xr","host_original_io_1190","831954a88faa0d058804adac1b2acf9da0265db2074f9ea31a21eab8a330bf9e",host_original_names_2,16u},
    {"tests/regression/10_stdlib/1191_io_edge.xr","host_original_io_1191","7152a2da6e15e44a8138afea5b1fc114caf7cbd1188be4d8e9d86a61d8534f79",host_original_names_3,14u},
    {"tests/regression/10_stdlib/1192_io_extended.xr","host_original_io_1192","d40ed9c06d78af71a22e7f4918258fcfa0109f5bdfadb31ad4c5dde2f7311005",host_original_names_4,7u},
    {"tests/regression/10_stdlib/1193_io_invalid_utf8.xr","host_original_io_1193_invalid_utf8","e39acbb15ff9db56c886c16b23005e325626ee1effcc2940b2c966052116fbe4",host_original_names_5,3u},
    {"tests/regression/10_stdlib/1193_io_read_dir_recursive.xr","host_original_io_1193_read_dir_recursive","5ad5a54af2dafd02713643d6e73844905627281273fc76003a5e0451017d00db",host_original_names_6,3u},
    {"tests/regression/10_stdlib/1194_io_mkdirp.xr","host_original_io_1194","f776cad5600965504813335248dd63fd5581395643e54720c8ac9fdf93b477b5",host_original_names_7,7u},
    {"tests/regression/10_stdlib/1195_io_remove_all.xr","host_original_io_1195","74e4638904e30b95c728986c1be46b27ad3720c27d6ccd7ebb866a16ccdfe8a8",host_original_names_8,5u},
    {"tests/regression/10_stdlib/1196_io_touch.xr","host_original_io_1196","85e0ac34e6e524d7f0f18a6aa60181549ed6a5e34e168c18e86c98934e2842fb",host_original_names_9,4u},
    {"tests/regression/10_stdlib/1197_io_temp.xr","host_original_io_1197","5351872976437f33dfc328b172748d9600869149596c72993ba6aa7d0ed402db",host_original_names_10,5u},
    {"tests/regression/10_stdlib/1198_io_invalid_utf8.xr","host_original_io_1198","e39acbb15ff9db56c886c16b23005e325626ee1effcc2940b2c966052116fbe4",host_original_names_11,3u},
};
typedef struct HostOriginalCatalog {
    HostOriginalWireRow *rows;
    uint32_t count;
    char source_sha256[65], canonical_sha256[65], generated_sha256[65];
} HostOriginalCatalog;
typedef enum HostOriginalStatus {
    HOST_ORIGINAL_NOT_RUN, HOST_ORIGINAL_PASSED, HOST_ORIGINAL_FAILED,
    HOST_ORIGINAL_ERROR, HOST_ORIGINAL_TIMEOUT, HOST_ORIGINAL_SKIPPED
} HostOriginalStatus;
typedef struct HostOriginalOutcome {
    HostOriginalStatus status;
    bool started;
    XrXirCallStatus terminal;
    char message[512];
} HostOriginalOutcome;
typedef struct HostOriginalHookEvent {
    uint32_t row; HostOriginalOutcome outcome;
} HostOriginalHookEvent;
typedef struct HostOriginalResult {
    unsigned index;
    const char *engine;
    const char *phase;
    HostOriginalCatalog catalog;
    HostOriginalOutcome *outcomes;
    HostOriginalHookEvent *hooks; size_t hook_count, hook_capacity;
    uint32_t source_status, source_stage;
    XrXirCallStatus cleanup;
    bool program_retired, owner_retained, extra_error;
    uint64_t held_before, held_after;
    size_t physical_before, bytes_before;
    char message[1024];
} HostOriginalResult;
static int host_original_suite_mode(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--original-suite")) return -1;
    if (argc != 4 || !argv[2][0] || !argv[3][0]) return -2;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)argv[2]; *p; ++p) {
        if (*p < '0' || *p > '9' || value > 11u) return -2;
        value = value * 10u + (unsigned)(*p - '0');
    }
    return value < HOST_ORIGINAL_FILE_COUNT ? (int)value : -2;
}
static void host_original_hex(const uint8_t digest[32], char output[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32u; ++i) {
        output[2u*i] = digits[digest[i] >> 4];
        output[2u*i+1u] = digits[digest[i] & 15u];
    }
    output[64] = 0;
}
static bool host_original_hash_file(const char *path, char raw[65], char canonical[65]) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    XrSHA256Context a, b; xr_sha256_init(&a); xr_sha256_init(&b);
    uint8_t buffer[4096]; size_t count; bool pending_cr = false;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0u) {
        xr_sha256_update(&a, buffer, count);
        for (size_t i = 0; i < count; ++i) {
            uint8_t c = buffer[i];
            if (pending_cr) {
                uint8_t previous = '\r';
                if (c != '\n') xr_sha256_update(&b, &previous, 1u);
                pending_cr = false;
            }
            if (c == '\r') pending_cr = true;
            else xr_sha256_update(&b, &c, 1u);
        }
    }
    if (pending_cr) { uint8_t c = '\r'; xr_sha256_update(&b, &c, 1u); }
    bool okay = !ferror(file); if (fclose(file)) okay = false;
    uint8_t digest[32]; xr_sha256_final(&a, digest); host_original_hex(digest, raw);
    xr_sha256_final(&b, digest); host_original_hex(digest, canonical);
    return okay;
}
static bool host_original_path(char *out, size_t capacity, unsigned index) {
    int n = snprintf(out, capacity, "%s/%s", XR_HOST_OS_ROOT, host_original_inputs[index].path);
    return n > 0 && (size_t)n < capacity;
}
static bool host_original_check_source(unsigned index, HostOriginalCatalog *catalog) {
    char path[4096];
    return host_original_path(path, sizeof(path), index) &&
        host_original_hash_file(path, catalog->source_sha256, catalog->canonical_sha256) &&
        !strcmp(catalog->canonical_sha256, host_original_inputs[index].canonical_sha256);
}
static void host_original_catalog_free(HostOriginalCatalog *catalog) {
    for (uint32_t i = 0; i < catalog->count; ++i) xr_free((void *)catalog->rows[i].name);
    xr_free(catalog->rows); catalog->rows = NULL; catalog->count = 0;
}
static bool host_original_catalog_allocate(HostOriginalCatalog *catalog, uint32_t count) {
    if (!count || (uint64_t)count > SIZE_MAX / sizeof(*catalog->rows)) return false;
    catalog->rows = xr_calloc(count, sizeof(*catalog->rows));
    if (!catalog->rows) return false;
    catalog->count = count; return true;
}
static bool host_original_row_copy(HostOriginalWireRow *out, uint32_t function,
    uint32_t role, uint32_t timeout, const char *name, size_t length) {
    if (!name || !length || length == SIZE_MAX || strlen(name) != length) return false;
    char *owned = xr_malloc(length + 1u); if (!owned) return false;
    memcpy(owned, name, length + 1u);
    *out = (HostOriginalWireRow){function, role, timeout, owned}; return true;
}
static bool host_original_catalog_valid(unsigned index, const HostOriginalCatalog *catalog) {
    const HostOriginalInput *input = host_original_inputs + index; uint32_t cases = 0;
    for (uint32_t i = 0; i < catalog->count; ++i) {
        const HostOriginalWireRow *row = catalog->rows + i;
        if (!row->name || row->role < XR_XIR_TEST_ROLE_TEST || row->role > XR_XIR_TEST_ROLE_AFTER_EACH) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (catalog->rows[j].function == row->function || !strcmp(catalog->rows[j].name, row->name)) return false;
        if (row->role == XR_XIR_TEST_ROLE_TEST || row->role == XR_XIR_TEST_ROLE_SKIP) {
            if (cases >= input->expected || strcmp(row->name, input->names[cases])) return false;
            ++cases;
        }
    }
    return cases == input->expected;
}
static bool host_original_now(uint64_t *out) {
    uint64_t value = xr_time_monotonic_ns(); if (!value) return false; *out = value; return true;
}
static XrXirOutputStatus host_original_output(void *context, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    (void)context;
    FILE *file = stream == XR_XIR_STDOUT ? stdout : stream == XR_XIR_STDERR ? stderr : NULL;
    if (!file) return XR_XIR_OUTPUT_BAD_ABI;
    return fwrite(bytes, 1u, length, file) == length && !fflush(file) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}
static int host_original_message(void *context, const void *bytes, size_t length) {
    char *message = context; size_t used = strlen(message), room = 511u - used;
    size_t copied = length < room ? length : room;
    if (copied) memcpy(message + used, bytes, copied);
    message[used + copied] = 0; return 1;
}
static void host_original_classify(HostOriginalOutcome *out, const XrXirCallResult *result,
    bool cancelled, bool clock_error) {
    out->terminal = result->status;
    if (clock_error) { out->status = HOST_ORIGINAL_ERROR; snprintf(out->message, sizeof(out->message), "host clock or wait failed"); }
    else if (cancelled) { out->status = HOST_ORIGINAL_TIMEOUT; snprintf(out->message, sizeof(out->message), "original test deadline exceeded"); }
    else if (result->status == XR_XIR_CALL_RETURNED) out->status = HOST_ORIGINAL_PASSED;
    else if (result->status == XR_XIR_CALL_THROWN || xr_xir_call_panic_status(result->status)) {
        out->status = HOST_ORIGINAL_FAILED;
        (void)xr_xir_host_report_result(result, (XrValueFormatSink){out->message, host_original_message}, false);
    } else { out->status = HOST_ORIGINAL_ERROR; snprintf(out->message, sizeof(out->message), "terminal status=%u", (unsigned)result->status); }
}
static HostOriginalOutcome host_original_invoke(XrXirInstance *instance, uint32_t function,
    uint64_t deadline) {
    HostOriginalOutcome out = {0}; out.status = HOST_ORIGINAL_ERROR;
    XrXirCallStatus status = xr_xir_instance_start_test(instance, function);
    if (status != XR_XIR_CALL_READY) {
        out.terminal = status;
        if (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED) {
            XrXirCallResult failure = {0};
            XrXirCallStatus copied = xr_xir_instance_copy_failure(instance, &failure);
            if (copied == failure.status && copied != XR_XIR_CALL_READY)
                host_original_classify(&out, &failure, false, false);
            xr_xir_call_result_drop(&failure);
        }
        if (!out.message[0]) snprintf(out.message, sizeof(out.message), "cannot start test status=%u", (unsigned)status);
        return out;
    }
    out.started = true; bool waiting = false, cancelled = false, host_failed = false;
    XrXirHostWait wait = {0}; uint64_t epoch = 0, wake = 0;
    for (;;) {
        if (waiting) {
            XrXirHostWaitStatus due = xr_xir_host_wait_poll(&wait, HOST_ORIGINAL_WAIT_NS);
            if (due == XR_XIR_HOST_WAIT_DUE) {
                status = xr_xir_instance_resume(instance, epoch, wake);
                if (status != XR_XIR_CALL_READY) host_failed = true;
                waiting = false;
            } else if (due != XR_XIR_HOST_WAIT_PENDING) host_failed = true;
        } else {
            XrXirInstanceResult slice = xr_xir_instance_poll_bounded(instance, 4096u);
            status = slice.outcome.status;
            if (status == XR_XIR_CALL_SUSPENDED) {
                XrXirWaitRequest request = {0}; epoch = slice.epoch; wake = slice.outcome.wake;
                if (xr_xir_instance_wait_request(instance, epoch, wake, &request) != XR_XIR_CALL_READY ||
                    xr_xir_host_wait_begin(&wait, &request) != XR_XIR_HOST_WAIT_PENDING) host_failed = true;
                else waiting = true;
            } else if (status != XR_XIR_CALL_READY) {
                host_original_classify(&out, &slice.outcome, cancelled, host_failed); return out;
            }
        }
        uint64_t now = 0; if (!host_original_now(&now)) host_failed = true;
        if (!cancelled && (host_failed || now >= deadline)) {
            waiting = false; status = xr_xir_instance_cancel_current(instance);
            if (status != XR_XIR_CALL_CANCEL_REQUESTED) {
                out.terminal = status; snprintf(out.message, sizeof(out.message), "cannot request cancellation status=%u", (unsigned)status); return out;
            }
            cancelled = true;
        }
        if (host_failed && cancelled) {
            XrXirInstanceResult drained;
            do drained = xr_xir_instance_poll_bounded(instance, 4096u);
            while (drained.outcome.status == XR_XIR_CALL_READY);
            host_original_classify(&out, &drained.outcome, true, true);
            return out;
        }
    }
}
static bool host_original_hooks(XrXirInstance *instance, const HostOriginalCatalog *catalog,
    uint32_t role, uint64_t deadline, HostOriginalResult *report) {
    bool okay = true;
    for (uint32_t i = 0; i < catalog->count; ++i) {
        if (catalog->rows[i].role != role) continue;
        HostOriginalOutcome result = host_original_invoke(instance, catalog->rows[i].function, deadline);
        if (report->hook_count == report->hook_capacity) {
            size_t capacity = report->hook_capacity ? report->hook_capacity * 2u : 8u;
            if (capacity <= report->hook_capacity || capacity > SIZE_MAX / sizeof(*report->hooks)) {
                report->extra_error = true; return false;
            }
            HostOriginalHookEvent *next = xr_realloc(report->hooks, capacity * sizeof(*next));
            if (!next) { report->extra_error = true; return false; }
            report->hooks = next; report->hook_capacity = capacity;
        }
        report->hooks[report->hook_count++] = (HostOriginalHookEvent){i, result};
        if (result.status != HOST_ORIGINAL_PASSED) {
            if (okay) snprintf(report->message, sizeof(report->message), "hook %s: %s", catalog->rows[i].name, result.message);
            okay = false; report->extra_error = true;
            if (role == XR_XIR_TEST_ROLE_BEFORE_ALL || role == XR_XIR_TEST_ROLE_BEFORE_EACH) break;
        }
    }
    return okay;
}
static void host_original_json_string(FILE *file, const char *text) {
    const unsigned char *p=(const unsigned char *)(text?text:"");size_t remaining=strlen((const char *)p);
    fputc('"',file);
    while(remaining){unsigned char c=*p;size_t width=1u;
        if(c=='"'||c=='\\'){fputc('\\',file);fputc(c,file);}
        else if(c<32u)fprintf(file,"\\u%04x",(unsigned)c);
        else if(c<128u)fputc(c,file);
        else {
            width=c>=0xc2u&&c<=0xdfu?2u:c>=0xe0u&&c<=0xefu?3u:c>=0xf0u&&c<=0xf4u?4u:0u;
            bool valid=width&&width<=remaining;
            for(size_t i=1u;valid&&i<width;++i)valid=(p[i]&0xc0u)==0x80u;
            if(valid&&width==3u)valid=!(c==0xe0u&&p[1]<0xa0u)&&!(c==0xedu&&p[1]>=0xa0u);
            if(valid&&width==4u)valid=!(c==0xf0u&&p[1]<0x90u)&&!(c==0xf4u&&p[1]>=0x90u);
            if(valid)fwrite(p,1u,width,file);else{fputs("\\ufffd",file);width=1u;}
        }
        p+=width;remaining-=width;
    }
    fputc('"',file);
}
static const char *host_original_status_name(HostOriginalStatus status) {
    static const char *const names[] = {"not_run","passed","failed","error","timeout","skipped"};
    return names[(unsigned)status < sizeof(names)/sizeof(*names) ? (unsigned)status : 3u];
}
/* Serializes before freeing copied rows; a retained instance keeps all owners live. */
static bool host_original_report(FILE *file, HostOriginalResult *r, bool release_rows) {
    const HostOriginalInput *input = host_original_inputs + r->index;
    uint32_t discovered = 0, started = 0, passed = 0, failed = 0, errors = 0, timeouts = 0, skipped = 0;
    for (uint32_t i = 0; i < r->catalog.count; ++i) {
        uint32_t role = r->catalog.rows[i].role;
        if (role != XR_XIR_TEST_ROLE_TEST && role != XR_XIR_TEST_ROLE_SKIP) continue;
        ++discovered;
        if (!r->outcomes) continue;
        HostOriginalOutcome *o = r->outcomes + i; started += o->started ? 1u : 0u;
        passed += o->status == HOST_ORIGINAL_PASSED ? 1u : 0u;
        failed += o->status == HOST_ORIGINAL_FAILED ? 1u : 0u;
        errors += o->status == HOST_ORIGINAL_ERROR ? 1u : 0u;
        timeouts += o->status == HOST_ORIGINAL_TIMEOUT ? 1u : 0u;
        skipped += o->status == HOST_ORIGINAL_SKIPPED ? 1u : 0u;
    }
    bool okay = discovered == input->expected && started == discovered && passed == discovered &&
        !failed && !errors && !timeouts && !skipped && !r->extra_error && r->program_retired &&
        !r->owner_retained && r->cleanup == XR_XIR_CALL_READY && r->held_before == r->held_after;
    fprintf(file,"{\"schema\":1,\"fileIndex\":%u,\"source\":",r->index);host_original_json_string(file,input->path);
    fputs(",\"engine\":",file);host_original_json_string(file,r->engine);
    fputs(",\"phase\":",file);host_original_json_string(file,r->phase);
    fprintf(file,",\"sourceSha256\":\"%s\",\"canonicalSourceSha256\":\"%s\",\"generatedCSha256\":\"%s\",\"sourceStatus\":%u,\"sourceStage\":%u,\"expected\":%u,\"discovered\":%u,\"executed\":%u,\"passed\":%u,\"failed\":%u,\"errors\":%u,\"timeouts\":%u,\"skipped\":%u,\"entries\":[",r->catalog.source_sha256,r->catalog.canonical_sha256,r->catalog.generated_sha256,r->source_status,r->source_stage,input->expected,discovered,started,passed,failed,errors,timeouts,skipped);
    for(uint32_t i=0;i<r->catalog.count;++i){const HostOriginalWireRow *row=r->catalog.rows+i;
        fprintf(file,"%s{\"name\":",i?",":"");host_original_json_string(file,row->name);
        fprintf(file,",\"function\":%u,\"role\":%u,\"timeout\":%u,\"status\":",row->function,row->role,row->timeout_seconds);
        host_original_json_string(file,r->outcomes?host_original_status_name(r->outcomes[i].status):"not_run");
        fprintf(file,",\"started\":%s,\"terminalStatus\":%u,\"message\":",r->outcomes&&r->outcomes[i].started?"true":"false",r->outcomes?(unsigned)r->outcomes[i].terminal:0u);
        host_original_json_string(file,r->outcomes?r->outcomes[i].message:"");fputc('}',file);
    }
    fputs("],\"hooks\":[",file);
    for(size_t i=0;i<r->hook_count;++i){const HostOriginalHookEvent *event=r->hooks+i;
        fprintf(file,"%s{\"row\":%u,\"name\":",i?",":"",event->row);
        host_original_json_string(file,r->catalog.rows[event->row].name);
        fputs(",\"status\":",file);host_original_json_string(file,host_original_status_name(event->outcome.status));
        fprintf(file,",\"started\":%s,\"terminalStatus\":%u,\"message\":",event->outcome.started?"true":"false",(unsigned)event->outcome.terminal);
        host_original_json_string(file,event->outcome.message);fputc('}',file);
    }
    fputs("],\"message\":",file);host_original_json_string(file,r->message);
    if(release_rows){host_original_catalog_free(&r->catalog);xr_free(r->outcomes);r->outcomes=NULL;xr_free(r->hooks);r->hooks=NULL;r->hook_count=r->hook_capacity=0;}
    bool physical=runtime_live==r->physical_before&&runtime_bytes==r->bytes_before;
    okay=okay&&physical;
    fprintf(file,",\"cleanupStatus\":%u,\"programRetired\":%s,\"ownerRetained\":%s,\"compilerHeldBefore\":%llu,\"compilerHeldAfter\":%llu,\"physicalRestored\":%s,\"status\":\"%s\"}\n",(unsigned)r->cleanup,r->program_retired?"true":"false",r->owner_retained?"true":"false",(unsigned long long)r->held_before,(unsigned long long)r->held_after,physical?"true":"false",okay?"pass":"fail");
    return !ferror(file)&&okay;
}
static bool host_original_run(XrXirProgram *program, HostOriginalResult *r, FILE *report_file) {
    r->outcomes=xr_calloc(r->catalog.count,sizeof(*r->outcomes));
    if(!r->outcomes){r->extra_error=true;snprintf(r->message,sizeof(r->message),"outcome allocation failed");return false;}
    XrXirInstanceConfig config;
    if(xr_xir_host_config_init(&config,sizeof(config),false)!=XR_XIR_CALL_READY){r->extra_error=true;return false;}
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,host_original_output,NULL,config.value_limit};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;
    XrXirCallStatus created=xr_xir_instance_new(program,&config,&instance);
    if(created!=XR_XIR_CALL_READY){r->extra_error=true;snprintf(r->message,sizeof(r->message),"instance creation status=%u",(unsigned)created);return false;}
    host_active=instance;
    uint64_t now=0,deadline=0;bool clock_ok=host_original_now(&now)&&now<=UINT64_MAX-HOST_ORIGINAL_FILE_NS;
    if(clock_ok)deadline=now+HOST_ORIGINAL_FILE_NS;
    else r->extra_error=true;
    bool setup=clock_ok&&host_original_hooks(instance,&r->catalog,XR_XIR_TEST_ROLE_BEFORE_ALL,deadline,r);
    for(uint32_t i=0;i<r->catalog.count;++i){
        const HostOriginalWireRow *row=r->catalog.rows+i;HostOriginalOutcome *out=r->outcomes+i;
        if(row->role==XR_XIR_TEST_ROLE_SKIP){out->status=HOST_ORIGINAL_SKIPPED;continue;}
        if(row->role!=XR_XIR_TEST_ROLE_TEST)continue;
        if(!setup||!host_original_now(&now)||now>=deadline){out->status=HOST_ORIGINAL_NOT_RUN;r->extra_error=true;continue;}
        if(host_original_hooks(instance,&r->catalog,XR_XIR_TEST_ROLE_BEFORE_EACH,deadline,r)) {
            uint64_t entry_deadline=deadline;
            uint64_t duration=(uint64_t)row->timeout_seconds*UINT64_C(1000000000);
            if(!host_original_now(&now)){out->status=HOST_ORIGINAL_ERROR;r->extra_error=true;}
            else {
                if(duration&&now<=UINT64_MAX-duration&&now+duration<entry_deadline)entry_deadline=now+duration;
                *out=host_original_invoke(instance,row->function,entry_deadline);
            }
        } else out->status=HOST_ORIGINAL_ERROR;
        bool after=host_original_hooks(instance,&r->catalog,XR_XIR_TEST_ROLE_AFTER_EACH,deadline,r);
        if(!after&&out->status==HOST_ORIGINAL_PASSED)out->status=HOST_ORIGINAL_ERROR;
    }
    if(clock_ok)(void)host_original_hooks(instance,&r->catalog,XR_XIR_TEST_ROLE_AFTER_ALL,deadline,r);
    r->cleanup=xr_xir_instance_free(instance);
    if(r->cleanup==XR_XIR_CALL_CLOSE_FAILED)r->cleanup=xr_xir_instance_free(instance);
    if(r->cleanup==XR_XIR_CALL_CLOSE_FAILED||r->cleanup==XR_XIR_CALL_BUSY){
        r->owner_retained=true;r->extra_error=true;r->phase="close_failed_owner_retained";
        (void)host_original_report(report_file,r,false);(void)fclose(report_file);fflush(stderr);
        /* The instance, sink, compiler ledger and code remain live until exit. */
        exit(1);
    }
    host_active=NULL;
    if(r->cleanup!=XR_XIR_CALL_READY)r->extra_error=true;
    return !r->extra_error;
}
#ifndef HOST_ORIGINAL_SUITE_SOURCE
extern const XrXirProgramSpec host_original_os_1160_program;
extern const HostOriginalWireRow host_original_os_1160_rows[];
extern const uint32_t host_original_os_1160_count;
extern const char host_original_os_1160_source_sha256[65];
extern const XrXirProgramSpec host_original_os_1161_program;
extern const HostOriginalWireRow host_original_os_1161_rows[];
extern const uint32_t host_original_os_1161_count;
extern const char host_original_os_1161_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1190_program;
extern const HostOriginalWireRow host_original_io_1190_rows[];
extern const uint32_t host_original_io_1190_count;
extern const char host_original_io_1190_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1191_program;
extern const HostOriginalWireRow host_original_io_1191_rows[];
extern const uint32_t host_original_io_1191_count;
extern const char host_original_io_1191_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1192_program;
extern const HostOriginalWireRow host_original_io_1192_rows[];
extern const uint32_t host_original_io_1192_count;
extern const char host_original_io_1192_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1193_invalid_utf8_program;
extern const HostOriginalWireRow host_original_io_1193_invalid_utf8_rows[];
extern const uint32_t host_original_io_1193_invalid_utf8_count;
extern const char host_original_io_1193_invalid_utf8_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1193_read_dir_recursive_program;
extern const HostOriginalWireRow host_original_io_1193_read_dir_recursive_rows[];
extern const uint32_t host_original_io_1193_read_dir_recursive_count;
extern const char host_original_io_1193_read_dir_recursive_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1194_program;
extern const HostOriginalWireRow host_original_io_1194_rows[];
extern const uint32_t host_original_io_1194_count;
extern const char host_original_io_1194_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1195_program;
extern const HostOriginalWireRow host_original_io_1195_rows[];
extern const uint32_t host_original_io_1195_count;
extern const char host_original_io_1195_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1196_program;
extern const HostOriginalWireRow host_original_io_1196_rows[];
extern const uint32_t host_original_io_1196_count;
extern const char host_original_io_1196_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1197_program;
extern const HostOriginalWireRow host_original_io_1197_rows[];
extern const uint32_t host_original_io_1197_count;
extern const char host_original_io_1197_source_sha256[65];
extern const XrXirProgramSpec host_original_io_1198_program;
extern const HostOriginalWireRow host_original_io_1198_rows[];
extern const uint32_t host_original_io_1198_count;
extern const char host_original_io_1198_source_sha256[65];
typedef struct HostOriginalNativeData {const XrXirProgramSpec *spec;const HostOriginalWireRow *rows;const uint32_t *count;const char *source_sha256;} HostOriginalNativeData;
static const HostOriginalNativeData host_original_native_data[] = {
    {&host_original_os_1160_program,host_original_os_1160_rows,&host_original_os_1160_count,host_original_os_1160_source_sha256},
    {&host_original_os_1161_program,host_original_os_1161_rows,&host_original_os_1161_count,host_original_os_1161_source_sha256},
    {&host_original_io_1190_program,host_original_io_1190_rows,&host_original_io_1190_count,host_original_io_1190_source_sha256},
    {&host_original_io_1191_program,host_original_io_1191_rows,&host_original_io_1191_count,host_original_io_1191_source_sha256},
    {&host_original_io_1192_program,host_original_io_1192_rows,&host_original_io_1192_count,host_original_io_1192_source_sha256},
    {&host_original_io_1193_invalid_utf8_program,host_original_io_1193_invalid_utf8_rows,&host_original_io_1193_invalid_utf8_count,host_original_io_1193_invalid_utf8_source_sha256},
    {&host_original_io_1193_read_dir_recursive_program,host_original_io_1193_read_dir_recursive_rows,&host_original_io_1193_read_dir_recursive_count,host_original_io_1193_read_dir_recursive_source_sha256},
    {&host_original_io_1194_program,host_original_io_1194_rows,&host_original_io_1194_count,host_original_io_1194_source_sha256},
    {&host_original_io_1195_program,host_original_io_1195_rows,&host_original_io_1195_count,host_original_io_1195_source_sha256},
    {&host_original_io_1196_program,host_original_io_1196_rows,&host_original_io_1196_count,host_original_io_1196_source_sha256},
    {&host_original_io_1197_program,host_original_io_1197_rows,&host_original_io_1197_count,host_original_io_1197_source_sha256},
    {&host_original_io_1198_program,host_original_io_1198_rows,&host_original_io_1198_count,host_original_io_1198_source_sha256},
};
static int host_original_native(unsigned index,const char *report_path){
    if(index>=HOST_ORIGINAL_FILE_COUNT)return 2;
#if defined(XR_OS_WINDOWS)
    if(_setmode(_fileno(stdout),_O_BINARY)==-1||_setmode(_fileno(stderr),_O_BINARY)==-1)return 1;
#endif
    FILE *file=fopen(report_path,"wbx");if(!file)return 2;
    HostOriginalResult r={0};r.index=index;r.engine="native";r.phase="input";r.cleanup=XR_XIR_CALL_READY;r.source_status=UINT32_MAX;
    r.physical_before=runtime_live;r.bytes_before=runtime_bytes;
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};XrXirProgram *program=NULL;
    bool okay=xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK;
    XrCompileResourceStats before={0},after={0};
    if(okay)okay=xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK;
    r.held_before=before.held_bytes;
    const HostOriginalNativeData *data=host_original_native_data+index;
    if(okay)okay=host_original_check_source(index,&r.catalog)&&!strcmp(r.catalog.source_sha256,data->source_sha256);
    if(okay)okay=*data->count>0u&&*data->count<=data->spec->entry_count&&host_original_catalog_allocate(&r.catalog,*data->count);
    for(uint32_t i=0;okay&&i<r.catalog.count;++i){const HostOriginalWireRow *row=data->rows+i;
        okay=host_original_row_copy(r.catalog.rows+i,row->function,row->role,row->timeout_seconds,row->name,strlen(row->name));}
    if(okay)okay=host_original_catalog_valid(index,&r.catalog);
    char generated[4096],unused[65];int n=snprintf(generated,sizeof(generated),"%s/%s.c",XR_HOST_OS_GENERATED,host_original_inputs[index].prefix);
    if(okay)okay=n>0&&(size_t)n<sizeof(generated)&&host_original_hash_file(generated,r.catalog.generated_sha256,unused);
    r.phase="program_seal";
    if(okay)okay=xr_xir_compile_program_seal(&context,data->spec,&program)==XR_XIR_OK;
    for(uint32_t i=0;okay&&i<r.catalog.count;++i){const HostOriginalWireRow *row=r.catalog.rows+i;
        okay=row->function<data->spec->entry_count;
        if(okay){const XrXirFunctionIdentity *identity=data->spec->declarations->functions+row->function;
            okay=identity->module==data->spec->declarations->root_module&&identity->test_role==row->role&&identity->test_timeout_seconds==row->timeout_seconds;}}
    if(okay){r.phase="executed";(void)host_original_run(program,&r,file);}else r.extra_error=true;
    xr_xir_compile_program_drop(program);r.program_retired=true;
    if(context.resources&&xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK)r.held_after=after.held_bytes;
    else r.extra_error=true;
    bool passed=host_original_report(file,&r,true);if(fclose(file))passed=false;
    if(context.resources)xr_compile_resources_release(context.resources);return passed?0:1;
}
#endif
#endif // HOST_OS_ORIGINAL_SUITE_RUNTIME_H
