"""Deterministic Windows readiness checks against complete generated C."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


HARNESS = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int64_t clock_frequency, samples[8];
static unsigned position, sample_count, sleeps, queries;
static int clock_frequency_ok, counter_failure;
static unsigned live_allocations, allocation_calls;
static void *tracked_malloc(size_t size) {
    void *result = malloc(size);
    if (result) { ++live_allocations; ++allocation_calls; }
    return result;
}
static void *tracked_calloc(size_t count, size_t size) {
    void *result = calloc(count, size);
    if (result) { ++live_allocations; ++allocation_calls; }
    return result;
}
static void *tracked_realloc(void *pointer, size_t size) {
    int fresh = pointer == NULL;
    if (!size) abort();
    void *result = realloc(pointer, size);
    if (result && fresh) { ++live_allocations; ++allocation_calls; }
    return result;
}
static void tracked_free(void *pointer) {
    if (pointer) {
        if (!live_allocations) abort();
        --live_allocations;
    }
    free(pointer);
}
static BOOL fake_clock_frequency(LARGE_INTEGER *value) {
    ++queries;
    value->QuadPart = clock_frequency;
    return clock_frequency_ok;
}
static BOOL fake_counter(LARGE_INTEGER *value) {
    ++queries;
    if ((int)position == counter_failure || position >= sample_count) return FALSE;
    value->QuadPart = samples[position++];
    return TRUE;
}
static void fake_sleep(DWORD milliseconds) {
    if (!milliseconds || milliseconds == INFINITE) abort();
    ++sleeps;
}
#define QueryPerformanceFrequency fake_clock_frequency
#define QueryPerformanceCounter fake_counter
#define Sleep fake_sleep
#define malloc tracked_malloc
#define calloc tracked_calloc
#define realloc tracked_realloc
#define free tracked_free
#define main generated_main
#include "generated.c"
#undef main
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)
static void reset(int64_t hz, int64_t first, int64_t second, int64_t third) {
    if (live_allocations) abort();
    allocation_calls = 0;
    clock_frequency = hz; clock_frequency_ok = 1; counter_failure = -1;
    samples[0] = first; samples[1] = second; samples[2] = third;
    sample_count = 3; position = sleeps = queries = 0;
}
int main(void) {
    reset(1000, 100, 119, 120);
    CHECK(generated_main() == 0 && sleeps == 2 && position == 3);
#ifdef XR_EXPECT_OWNED_TIMER
    CHECK(allocation_calls > 0 && live_allocations == 0);
#endif
    reset(1000, 100, 120, 120);
    CHECK(xr_aot_host_wait_timer(20) && sleeps == 1);
    reset(3, 10, 10, 11);
    CHECK(xr_aot_host_wait_timer(1) && sleeps == 2);
    reset(INT64_MAX, 0, INT64_MAX - 1, INT64_MAX);
    CHECK(xr_aot_host_wait_timer(1000) && sleeps == 2);
    reset(INT64_MAX, 0, INT64_MAX / 2, INT64_MAX - 1);
    CHECK(xr_aot_host_wait_timer(999) && sleeps == 2);
    reset(1000, INT64_MAX - 20, INT64_MAX - 1, INT64_MAX);
    CHECK(xr_aot_host_wait_timer(20) && sleeps == 2);
    reset(1000, 100, 86399999 + 100, 86400000 + 100);
    CHECK(xr_aot_host_wait_timer(86400000) && sleeps == 2);
    reset(1000, 0, 0, 0);
    CHECK(xr_aot_host_wait_timer(0) && queries == 0 && sleeps == 0);
    CHECK(!xr_aot_host_wait_timer(-1) && !xr_aot_host_wait_timer(86400001));
    CHECK(!xr_aot_host_wait_timer(INT64_MIN) && !xr_aot_host_wait_timer(INT64_MAX));
    CHECK(queries == 0 && sleeps == 0);
    reset(0, 100, 120, 120);
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 0);
    reset(-1, 100, 120, 120);
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 0);
    reset(1000, 100, 120, 120); clock_frequency_ok = 0;
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 0);
    reset(1000, -1, 120, 120);
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 0);
    reset(1000, 100, 120, 120); counter_failure = 0;
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 0);
    reset(1000, 100, 120, 120); counter_failure = 1;
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 1);
    reset(1000, 100, 110, 109);
    CHECK(!xr_aot_host_wait_timer(20) && sleeps == 2);
    reset(1000, 100, 120, 120); counter_failure = 1;
    CHECK(generated_main() == 214 && sleeps == 1);
#ifdef XR_EXPECT_OWNED_TIMER
    CHECK(allocation_calls > 0 && live_allocations == 0);
#endif
    reset(1000, 100, 120, 120); clock_frequency_ok = 0;
    XrAotContext context = {0};
    XrAotModules modules = {0};
    modules.storage.modules = &modules; context.modules = &modules;
    XrAotOutcome result = xr_aot_initialize_modules(&context);
    CHECK(result.kind == 1 && result.trap == 4 && queries == 1);
    clock_frequency_ok = 1;
    result = xr_aot_initialize_modules(&context);
    CHECK(result.kind == 1 && result.trap == 4 && queries == 1);
    xr_aot_modules_destroy(&modules);
    CHECK(live_allocations == 0);
    return 0;
}
'''


def run(command: list[str], root: Path) -> None:
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError(f"{command}: {result.returncode}\n"
                             f"{result.stdout!r}\n{result.stderr!r}")


def check_native_timer_entry(writer: Path, compiler: str, root: Path) -> None:
    generated = root / "generated.c"
    run([str(writer.resolve()), str(generated), "coroutine-timer"], root)
    prefix = HARNESS[:HARNESS.index("int main(void) {")]
    prefix = prefix.replace('#define main generated_main',
                            'static unsigned cancel_publications;\n#define main generated_main')
    body = r'''
int main(void) {
    reset(1000, 100, 109, 110);
    CHECK(generated_main() == 12 && sleeps == 2 && position == 3);
    CHECK(cancel_publications == 0);
    reset(1000, 100, 110, 110); clock_frequency_ok = 0;
    CHECK(generated_main() == 214 && sleeps == 0);
#ifdef XR_OBSERVE_CANCEL
    CHECK(cancel_publications == 1);
#endif
    cancel_publications = 0;
    reset(1000, 100, 109, 108);
    CHECK(generated_main() == 214 && sleeps == 2);
#ifdef XR_OBSERVE_CANCEL
    CHECK(cancel_publications == 1);
#endif
    CHECK(live_allocations == 0);
    return 0;
}
'''
    (root / "harness.c").write_text(prefix + body, encoding="utf-8")
    command = [compiler, "/nologo", "/std:c11", "/W4", "/WX", "/wd4505",
               "harness.c", "/Fe:timer.exe"]
    run(command, root)
    run([str(root / "timer.exe")], root)
    # First execute untouched output. Then add only a counter at the generated
    # cancellation publication to observe selection of the verified cancel edge.
    source = generated.read_text(encoding="utf-8")
    publication = "return xr_aot_make(6, 0, 0);"
    if source.count(publication) != 1:
        raise AssertionError("timer entry fixture must have exactly one cancel publication")
    generated.write_text(source.replace(publication, "++cancel_publications; " + publication),
                         encoding="utf-8")
    run([*command, "/DXR_OBSERVE_CANCEL"], root)
    run([str(root / "timer.exe")], root)


def check_native_timer_readiness(binary: Path, fixture: Path, writer: Path) -> None:
    if os.name != "nt":
        with tempfile.TemporaryDirectory(prefix="xray-native-timer-") as temporary:
            root = Path(temporary)
            executable = root / "timer"
            run([str(binary.resolve()), "build", "--native", str(fixture.resolve()),
                 "-o", str(executable)], root)
            started = time.perf_counter()
            run([str(executable)], root)
            elapsed = time.perf_counter() - started
            if elapsed < 0.020:
                raise AssertionError(f"native timer returned early: {elapsed:.9f}s")
        print("Native timer elapsed check: PASS; Windows clock injection: NOT_APPLICABLE")
        return
    compiler = shutil.which("cl")
    if compiler is None:
        raise AssertionError("Windows timer regression requires the ready MSVC C11 provider")
    with tempfile.TemporaryDirectory(prefix="xray-native-timer-") as temporary:
        root = Path(temporary)
        generated = root / "generated.c"
        owned = root / "owned.xr"
        owned.write_text(
            'import time\n'
            'fn wait() -> string { var number:i64=42; var text=number.toString(); '
            'time.sleep(20); return text }\n'
            'var retained=wait()\nassert(retained == "42")\n', encoding="utf-8")
        (root / "harness.c").write_text(HARNESS, encoding="utf-8")
        for source in (fixture, owned):
            commands = [
                [str(binary.resolve()), "build", "--c-only", str(source.resolve()),
                 "-o", str(generated)],
                [compiler, "/nologo", "/std:c11", "/W4", "/WX", "/wd4505",
                 *(["/DXR_EXPECT_OWNED_TIMER"] if source == owned else []),
                 "harness.c", "/Fe:timer.exe"],
                [str(root / "timer.exe")],
            ]
            for command in commands:
                run(command, root)
            if "({" in generated.read_text(encoding="utf-8"):
                raise AssertionError("generated timer contains a statement expression")
        check_native_timer_entry(writer, compiler, root)
    print("Windows injected native timer: PASS")


if __name__ == "__main__":
    import sys
    check_native_timer_readiness(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
