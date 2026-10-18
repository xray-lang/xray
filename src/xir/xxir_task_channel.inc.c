/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_channel.inc.c - Charged discovery of owned Channel readiness
 *
 * KEY CONCEPT:
 *   Only executor-owned activations enter its FIFO; no queue borrows a frame.
 */
static void task_core_scan_channel(XrXirTaskExecutor *executor, XirTaskActivation *activation,
    bool permitted) {
    if (!activation->call || activation->queued || !xr_xir_call_channel_waiting(activation->call)) return;
    XrXirCallStatus failure=task_core_completion_status(executor);
    if (!permitted && failure==XR_XIR_CALL_READY) failure=XR_XIR_CALL_LIMIT;
    const XrXirExecutorBinding binding = {executor, activation, executor->generation, activation->ticket};
    if (failure==XR_XIR_CALL_READY) failure=xr_xir_call_channel_deadline_poll(activation->call,&binding);
    if (failure!=XR_XIR_CALL_READY) {
        XR_CHECK(xr_xir_call_fail_channel_wait(activation->call, &binding, failure) == XR_XIR_CALL_READY,
            "an accepted epoch failure removes only the exact executor-owned Channel wait");
        task_core_record_failure(executor, activation); task_core_enqueue(executor, activation);
    } else if (xr_xir_call_channel_ready(activation->call)) task_core_enqueue(executor, activation);
    else executor->channel_waiting = true;
}
static void task_core_scan_channels(XrXirTaskExecutor *executor) {
    if (!executor->channel_waiting) return;
    executor->channel_waiting = false;
    if (executor->root.call) task_core_scan_channel(executor, &executor->root,
        xr_xir_domain_work(executor->config.domain, 1));
    for (XirTask *task = executor->active; task; task = task->active_next)
        task_core_scan_channel(executor, &task->activation, xr_xir_domain_work(executor->config.domain, 1));
}
