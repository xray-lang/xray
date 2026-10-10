/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_receiver_normal_cleanup.inc.c - Original current receiver cleanup body
 *
 * KEY CONCEPT:
 *   Every ordinary, serial, dual and private class owner keeps its current status-sensitive cleanup.
 */
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) {
            receiver_cleanup_failed = true;
            fprintf(stderr, "cleanup-error Instance=%u BUSY, physical cleanup unproved\n", i);
        } else {
            run->instances[i] = NULL;
            if (status != XR_XIR_CALL_READY) {
                receiver_cleanup_failed = true;
                record_failure("PRODUCT_CALL_STATUS", "failure-cleanup-Instance-free", true, XR_XIR_CALL_READY, status, NULL);
            }
        }
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
#if defined(XR_SOURCE_TYPED_LIBRARY_CLASS_OBSERVATION_PRIVATE24) && !defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
    release_class_observations(run);
#endif
    if (receiver_native_owner && !run->instances[0] && !run->instances[1]) {
        receiver_cleanup_failed = true;
        ReceiverNativeOwner *owner = receiver_native_owner; receiver_native_owner = NULL;
        dispose_receiver_native_owner(owner);
    }
    if (run->pending_native_owner) {
        ReceiverNativeOwner *owner = run->pending_native_owner;
        dispose_receiver_native_owner(owner); run->pending_native_owner = NULL;
    }
    free_poisoned_packet(&run->input_packet);
    poison(run->input_identity, sizeof(run->input_identity)); poison(run->proof_identity, sizeof(run->proof_identity));
    poison(run->packet_path, sizeof(run->packet_path)); poison(run->identity_path, sizeof(run->identity_path));
