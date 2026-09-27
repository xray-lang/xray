/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_timer.inc.c - Standalone native timer readiness driver.
 */

static bool emit_host_timer(CBuffer *buffer) {
    return append_text(buffer,
        "static int xr_aot_host_wait_timer(int64_t milliseconds) {\n"
        "    if (milliseconds < 0 || milliseconds > INT64_C(86400000)) return 0;\n"
        "    if (milliseconds == 0) return 1;\n"
        "#if defined(_WIN32)\n"
        "    LARGE_INTEGER frequency, start, now;\n"
        "    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||\n"
        "        !QueryPerformanceCounter(&start) || start.QuadPart < 0) return 0;\n"
        "    uint64_t hz = (uint64_t)frequency.QuadPart;\n"
        "    uint64_t seconds = (uint64_t)milliseconds / UINT64_C(1000);\n"
        "    uint64_t remainder = (uint64_t)milliseconds % UINT64_C(1000);\n"
        "    /* ceil(hz * remainder / 1000), without overflowing the product. */\n"
        "    uint64_t ticks = (hz / UINT64_C(1000)) * remainder +\n"
        "        ((hz % UINT64_C(1000)) * remainder + UINT64_C(999)) / UINT64_C(1000);\n"
        "    int64_t previous = start.QuadPart;\n"
        "    DWORD delay = (DWORD)milliseconds;\n"
        "    for (;;) {\n"
        "        Sleep(delay);\n"
        "        if (!QueryPerformanceCounter(&now) || now.QuadPart < previous) return 0;\n"
        "        previous = now.QuadPart;\n"
        "        uint64_t elapsed = (uint64_t)now.QuadPart - (uint64_t)start.QuadPart;\n"
        "        uint64_t whole = elapsed / hz;\n"
        "        if (whole > seconds || (whole == seconds && elapsed % hz >= ticks)) return 1;\n"
        "        /* An early wakeup cannot grant readiness; retry at finite resolution. */\n"
        "        delay = 1;\n"
        "    }\n"
        "#else\n"
        "    struct timespec delay;\n"
        "    delay.tv_sec = (time_t)(milliseconds / INT64_C(1000));\n"
        "    delay.tv_nsec = (long)((milliseconds % INT64_C(1000)) * INT64_C(1000000));\n"
        "    while (nanosleep(&delay, &delay) != 0) {\n"
        "        if (errno != EINTR) return 0;\n"
        "    }\n"
        "    return 1;\n"
        "#endif\n"
        "}\n"
        "\n"
    );
}
