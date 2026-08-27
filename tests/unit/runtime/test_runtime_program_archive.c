/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_program_archive.c - Public program lifecycle boundary
 *
 * KEY CONCEPT:
 *   The bounded program route is a public facade with no entry cell to close,
 *   so the program handle itself is what decides quiescence. These cases hold
 *   it to that: concurrent executions agree, an unload racing them is refused
 *   rather than tearing them down, and a refused unload leaves the program
 *   callable. Linking only the runtime archive keeps the proof on the product
 *   boundary.
 */

#include "xray_runtime_api.h"

#include "os/os_thread.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

#define PROGRAM_RESULT 42

typedef struct Artifact {
    uint8_t *bytes;
    size_t size;
} Artifact;

static Artifact read_artifact(const char *name) {
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s", XR_RUNTIME_PROGRAM_ARTIFACT_DIR, name);
    REQUIRE(written > 0 && (size_t) written < sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "program artifact fixture is missing: %s\n", path);
        abort();
    }
    REQUIRE(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    REQUIRE(length > 0);
    REQUIRE(fseek(file, 0, SEEK_SET) == 0);
    Artifact artifact = {.bytes = (uint8_t *) malloc((size_t) length), .size = (size_t) length};
    REQUIRE(artifact.bytes != NULL);
    REQUIRE(fread(artifact.bytes, 1, artifact.size, file) == artifact.size);
    fclose(file);
    return artifact;
}

static void free_artifact(Artifact *artifact) {
    free(artifact->bytes);
    artifact->bytes = NULL;
    artifact->size = 0;
}

static void *runtime_allocate(void *context, size_t size, size_t alignment) {
    (void) context;
    (void) alignment;
    return malloc(size);
}

static void runtime_deallocate(void *context, void *allocation, size_t size, size_t alignment) {
    (void) context;
    (void) size;
    (void) alignment;
    free(allocation);
}

static void runtime_panic(void *context, const char *message, size_t message_size) {
    (void) context;
    (void) message;
    (void) message_size;
    abort();
}

static XrRuntime *make_runtime(void) {
    char diagnostic[512] = {0};
    XrRuntimeConfig config = {
        .schema_version = XR_RUNTIME_CONFIG_SCHEMA_VERSION,
        .generation =
            {
                .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
                .max_loaded_generations = 2u,
                .max_total_pins = 32u,
                .max_pins_per_generation = 16u,
                .max_pins_by_kind = {16u, 16u, 4u, 4u, 4u},
            },
        .activation =
            {
                .max_active_entries = 8u,
                .max_active_provider_registrations = 8u,
                .max_active_finalizer_registrations = 4u,
            },
        .providers =
            {
                .allocate = runtime_allocate,
                .deallocate = runtime_deallocate,
                .panic = runtime_panic,
            },
    };
    XrRuntime *runtime = NULL;
    if (!xr_runtime_create(&config, &runtime, diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "runtime create failed: %s\n", diagnostic);
        abort();
    }
    return runtime;
}


typedef struct ProgramFixture {
    Artifact modules[2];
    Artifact target;
} ProgramFixture;

static ProgramFixture load_fixture(void) {
    ProgramFixture fixture = {
        .modules = {read_artifact("runtime_program_0.xsm"),
                    read_artifact("runtime_program_1.xsm")},
        .target = read_artifact("runtime_program.xtp"),
    };
    return fixture;
}

static void free_fixture(ProgramFixture *fixture) {
    free_artifact(&fixture->modules[0]);
    free_artifact(&fixture->modules[1]);
    free_artifact(&fixture->target);
}

static XrProgram *load_program(XrRuntime *runtime, const ProgramFixture *fixture, bool reversed) {
    char diagnostic[512] = {0};
    XrRuntimeArtifactImage images[2] = {
        {.bytes = fixture->modules[reversed ? 1 : 0].bytes,
         .size = fixture->modules[reversed ? 1 : 0].size},
        {.bytes = fixture->modules[reversed ? 0 : 1].bytes,
         .size = fixture->modules[reversed ? 0 : 1].size},
    };
    XrProgram *program = NULL;
    if (!xr_program_load_target_plan(runtime, images, 2u, fixture->target.bytes,
                                     fixture->target.size, &program, diagnostic,
                                     sizeof(diagnostic))) {
        fprintf(stderr, "program load failed: %s\n", diagnostic);
        abort();
    }
    return program;
}

/* ========== concurrent execution agrees ========== */

#define EXECUTE_THREADS 4u
#define EXECUTE_ROUNDS 64u

typedef struct ExecuteWorker {
    const XrProgram *program;
    uint32_t executed;
    uint32_t refused;
    bool wrong_result;
    char diagnostic[256];
} ExecuteWorker;

static void *execute_worker(void *opaque) {
    ExecuteWorker *worker = (ExecuteWorker *) opaque;
    for (uint32_t round = 0; round < EXECUTE_ROUNDS; round++) {
        int64_t result = 0;
        if (xr_program_execute_direct_i64(worker->program, &result, worker->diagnostic,
                                          sizeof(worker->diagnostic))) {
            worker->executed++;
            if (result != PROGRAM_RESULT)
                worker->wrong_result = true;
        } else {
            worker->refused++;
        }
    }
    return NULL;
}

static void test_concurrent_execution_agrees(const ProgramFixture *fixture) {
    char diagnostic[512] = {0};
    XrRuntime *runtime = make_runtime();
    XrProgram *program = load_program(runtime, fixture, false);

    ExecuteWorker workers[EXECUTE_THREADS] = {0};
    xr_thread_t threads[EXECUTE_THREADS];
    for (uint32_t i = 0; i < EXECUTE_THREADS; i++) {
        workers[i].program = program;
        REQUIRE(xr_thread_create(&threads[i], execute_worker, &workers[i]));
    }
    for (uint32_t i = 0; i < EXECUTE_THREADS; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        REQUIRE(!workers[i].wrong_result);
        /* Every call rechecks the live manifest and takes its own in-flight
         * pin, so concurrency may exhaust the pin budget but may never produce
         * a different verified result. */
        REQUIRE(workers[i].executed + workers[i].refused == EXECUTE_ROUNDS);
        REQUIRE(workers[i].executed != 0);
    }

    /* Joined callers leave the program quiescent, which is the only state
     * unload accepts. */
    REQUIRE(xr_program_unload(&program, diagnostic, sizeof(diagnostic)));
    REQUIRE(program == NULL);
    REQUIRE(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)));
    puts("  concurrent execution agrees");
}

/* ========== unload is quiescence gated ==========
 *
 * Unload decides quiescence before it touches a single teardown step, so an
 * attempt that lands while a call is in flight is refused with the program
 * untouched. Refusals are therefore the expected outcome here, and each one
 * must name the quiescence gate rather than a torn-down lifecycle. */

typedef struct RacingWorker {
    const XrProgram *program;
    volatile uint32_t *stop;
    uint32_t executed;
    uint32_t refused;
    bool wrong_result;
} RacingWorker;

static void *racing_execute_worker(void *opaque) {
    RacingWorker *worker = (RacingWorker *) opaque;
    for (uint32_t round = 0; round < EXECUTE_ROUNDS && !*worker->stop; round++) {
        int64_t result = 0;
        char diagnostic[256] = {0};
        if (xr_program_execute_direct_i64(worker->program, &result, diagnostic,
                                          sizeof(diagnostic))) {
            worker->executed++;
            if (result != PROGRAM_RESULT)
                worker->wrong_result = true;
        } else {
            worker->refused++;
        }
    }
    return NULL;
}

static void test_unload_is_quiescence_gated(const ProgramFixture *fixture) {
    char diagnostic[512] = {0};
    XrRuntime *runtime = make_runtime();
    XrProgram *program = load_program(runtime, fixture, false);

    volatile uint32_t stop = 0;
    RacingWorker workers[EXECUTE_THREADS] = {0};
    xr_thread_t threads[EXECUTE_THREADS];
    for (uint32_t i = 0; i < EXECUTE_THREADS; i++) {
        workers[i].program = program;
        workers[i].stop = &stop;
        REQUIRE(xr_thread_create(&threads[i], racing_execute_worker, &workers[i]));
    }

    uint32_t refused_unloads = 0;
    bool unloaded = false;
    for (uint32_t attempt = 0; attempt < 256u && !unloaded; attempt++) {
        char attempt_diagnostic[256] = {0};
        if (xr_program_unload(&program, attempt_diagnostic, sizeof(attempt_diagnostic))) {
            unloaded = true;
            stop = 1;
            break;
        }
        refused_unloads++;
        /* A refusal is the quiescence gate, never a half-completed teardown. */
        REQUIRE(strstr(attempt_diagnostic, "XR_EXEC_5006") != NULL);
        REQUIRE(program != NULL);
    }
    stop = 1;
    for (uint32_t i = 0; i < EXECUTE_THREADS; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        REQUIRE(!workers[i].wrong_result);
    }
    /* Whether or not an attempt found a gap, the program is still exactly one
     * unload away from gone and never became unusable in between. */
    if (!unloaded) {
        REQUIRE(refused_unloads != 0);
        int64_t result = 0;
        REQUIRE(xr_program_execute_direct_i64(program, &result, diagnostic, sizeof(diagnostic)));
        REQUIRE(result == PROGRAM_RESULT);
        REQUIRE(xr_program_unload(&program, diagnostic, sizeof(diagnostic)));
    }
    REQUIRE(program == NULL);
    REQUIRE(!xr_program_unload(&program, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)));
    puts("  unload is quiescence gated");
}

/* ========== reordered module set binds the same program ========== */

static void test_reordered_module_set_binds_the_same_program(const ProgramFixture *fixture) {
    char diagnostic[512] = {0};
    for (uint32_t reversed = 0; reversed < 2u; reversed++) {
        XrRuntime *runtime = make_runtime();
        XrProgram *program = load_program(runtime, fixture, reversed != 0);
        int64_t result = 0;
        REQUIRE(xr_program_execute_direct_i64(program, &result, diagnostic, sizeof(diagnostic)));
        REQUIRE(result == PROGRAM_RESULT);
        REQUIRE(xr_program_unload(&program, diagnostic, sizeof(diagnostic)));
        REQUIRE(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)));
    }
    puts("  reordered module set binds the same program");
}

/* ========== runtime destroy refuses a live program, and reload is exact ========== */

static void test_runtime_boundary_and_reload(const ProgramFixture *fixture) {
    char diagnostic[512] = {0};
    XrRuntime *runtime = make_runtime();
    XrProgram *program = load_program(runtime, fixture, false);

    /* The runtime owns its loaded artifacts, so it refuses to go away under
     * one and keeps its handle. */
    REQUIRE(!xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5006") != NULL);
    REQUIRE(runtime != NULL);
    int64_t result = 0;
    REQUIRE(xr_program_execute_direct_i64(program, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == PROGRAM_RESULT);
    REQUIRE(xr_program_unload(&program, diagnostic, sizeof(diagnostic)));

    /* The same exact identity loads again into a fresh handle and a fresh
     * generation; nothing from the unloaded one is reused. */
    XrProgram *reloaded = load_program(runtime, fixture, false);
    REQUIRE(reloaded != NULL);
    result = 0;
    REQUIRE(xr_program_execute_direct_i64(reloaded, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == PROGRAM_RESULT);
    REQUIRE(xr_program_unload(&reloaded, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)));
    puts("  runtime boundary and reload are exact");
}

int main(void) {
    ProgramFixture fixture = load_fixture();
    test_concurrent_execution_agrees(&fixture);
    test_unload_is_quiescence_gated(&fixture);
    test_reordered_module_set_binds_the_same_program(&fixture);
    test_runtime_boundary_and_reload(&fixture);
    free_fixture(&fixture);
    puts("runtime program archive tests passed");
    return 0;
}
