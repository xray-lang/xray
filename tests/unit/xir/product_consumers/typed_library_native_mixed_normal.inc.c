/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_native_mixed_normal.inc.c - Original ordinary Instance execution
 *
 * KEY CONCEPT:
 *   The original function is included verbatim under its ordinary receiver guard.
 */
static void run_receiver_instances(ReceiverRun *run, const char *c_path) {
    seal_receiver_native(run, c_path);
    XrXirInstanceConfig config;
    CHECK(call_completed(run, xr_xir_instance_config_init(&config, sizeof(config)), XR_XIR_CALL_READY, "xr-xir-instance-config-init"));
    config.value_limit = UINT64_C(1048576);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(call_completed(run, xr_xir_instance_new(run->program, &config, &run->instances[i]), XR_XIR_CALL_READY, "Instance-new") && run->instances[i]);
    CHECK(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    CHECK(!receiver_observer_failed && receiver_native_owner && !receiver_native_releases);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(call_completed(run, xr_xir_instance_start(run->instances[i], run->roles.private_answer, NULL, 0), XR_XIR_CALL_BAD_ARGUMENT, "private-answer-permission-reject"));
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_receiver_i64(run, run->instances[i], run->roles.entry, 0, 0);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            NativeRouteCounts before = native_route_counts(run->roles);
            execute_receiver_i64(run, run->instances[i], run->roles.answer, 42, 2);
            NativeRouteCounts after = native_route_counts(run->roles);
            check_actual_route(run->mode, before, after);
            printf("typed-library-native-mixed instance=%u repeat=%u mode=%u constructor-native=%llu constructor-VM=%llu child-native=%llu child-VM=%llu actual-yield-resumes=2 owned-I64=42\n",
                i, repeat, run->mode, (unsigned long long)(after.native[0] - before.native[0]),
                (unsigned long long)(after.vm[0] - before.vm[0]), (unsigned long long)(after.native[1] - before.native[1]),
                (unsigned long long)(after.vm[1] - before.vm[1]));
        }
        /* READY free applies only to these completely successful normal calls. */
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "successful-normal-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_native_owner && !receiver_native_releases);
    }
    CHECK(!receiver_native_owner && receiver_native_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}
