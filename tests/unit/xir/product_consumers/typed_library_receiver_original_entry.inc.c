/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_receiver_original_entry.inc.c - Original current consumer entry body
 *
 * KEY CONCEPT:
 *   Writer, ordinary, serial, dual and private class selections retain the exact current entry.
 */
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    if (argc != 8) return 2;
#else
    if (argc != 5) return 2;
#endif
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    /* Static state preserves every owned pointer across the test's failure jump. */
    static ReceiverRun run;
#if !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    run.mode = !strcmp(argv[1], "1") ? 1u : !strcmp(argv[1], "2") ? 2u : !strcmp(argv[1], "3") ? 3u : 0u;
    if (!run.mode) return 2;
#endif
    run.context = source_program_owner(67108864, 128000000);
    volatile bool passed = false;
    if (setjmp(receiver_failure) == 0) {
        receiver_cleanup_enabled = true;
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
        copy_owned_sources(&run, argv);
        passed = library_catalog_build(&run) && program_source_build(&run) && detach_program(&run, argv);
#else
        passed = admit_packet(&run, argv);
#if defined(XR_SOURCE_TYPED_LIBRARY_CLASS_OBSERVATION_PRIVATE24)
        if (passed) run_receiver_class_observation_instances(&run, argv[4]);
#else
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_DUAL_PENDING_RECEIVER)
        if (passed) run_receiver_dual_pending_instances(&run, argv[4]);
#elif defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
        if (passed) run_receiver_cancel_instances(&run, argv[4]);
#else
        if (passed) run_receiver_instances(&run, argv[4]);
#endif
#endif
#endif
    } else passed = false;
    receiver_cleanup_enabled = false;
    release_remaining(&run);
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    if (!passed || receiver_cleanup_failed || receiver_first_failure.domain) cleanup_failed_materials(&run);
#endif
    if (receiver_first_failure.domain) fprintf(stderr,
        "typed-library-receiver first-domain=%s operation=%s status-observed=%u expected=%d actual=%d line=%d expression=%s\n",
        receiver_first_failure.domain, receiver_first_failure.operation, (unsigned)receiver_first_failure.has_status,
        receiver_first_failure.expected, receiver_first_failure.actual, receiver_first_failure.line,
        receiver_first_failure.expression ? receiver_first_failure.expression : "(status-return)");
    source_program_owners_free();
    bool physical_zero = !source_program_compile_live && !source_program_compile_bytes &&
        !source_program_compile_allocations && !source_program_compile_capacity &&
        !runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity;
    if (!physical_zero) fprintf(stderr, "cleanup-error physical-zero-unproved compile=%zu/%zu table=%p/%zu runtime=%zu/%zu table=%p/%zu\n",
        source_program_compile_live, source_program_compile_bytes, (void *)source_program_compile_allocations,
        source_program_compile_capacity, runtime_live, runtime_bytes, (void *)runtime_owned, runtime_owned_capacity);
    if (!physical_zero || receiver_cleanup_failed || receiver_observer_failed) passed = false;
#if defined(XR_SOURCE_TYPED_LIBRARY_CLASS_OBSERVATION_PRIVATE24) && !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    report_class_observation_execution(&run, passed, physical_zero);
#else
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_DUAL_PENDING_RECEIVER)
    report_dual_pending_execution(&run, passed, physical_zero);
#elif defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
    report_cancel_execution(&run, passed, physical_zero);
#else
    printf("typed-library-native-mixed-packet last-XIR-status=%u result=%s cleanup-error=%u physical-zero=%u full-FI=NOT_RUN axes=NOT_RUN cancel=NOT_RUN stable-object-lifecycle=OPEN init-failure=OPEN owned-escape=OPEN original-AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN\n",
        (unsigned)run.status, passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero);
#endif
#endif
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    if (passed) puts("typed-Library-packet writer-runtime=NOT_RUN Source-and-Catalog-producers-released=1");
    else puts("typed-Library-packet writer-runtime=NOT_RUN material-publication=FAIL");
#endif
    return passed ? 0 : 1;
