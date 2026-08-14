/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pass_policy.c - Optimizer policy for a compilation session
 */

#include "xi_pass_policy.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xplatform.h"
#include "../os/os_thread.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Names ========== */

static const char *const xi_pass_names[] = {
#define XI_OPT_PASS_NAME_ENTRY(upper, lower) lower,
    XI_OPT_PASS_LIST(XI_OPT_PASS_NAME_ENTRY)
#undef XI_OPT_PASS_NAME_ENTRY
};

XR_STATIC_ASSERT(sizeof(xi_pass_names) / sizeof(xi_pass_names[0]) == (size_t) XI_OPT_PASS_ID_COUNT,
                 "pass name table and pass id list disagree");

XR_FUNC const char *xi_pass_name_by_id(int pass_id) {
    if (pass_id < 0 || pass_id >= (int) XI_OPT_PASS_ID_COUNT)
        return NULL;
    return xi_pass_names[pass_id];
}

XR_FUNC int xi_pass_id_by_name(const char *name) {
    if (!name || !name[0])
        return -1;
    for (int i = 0; i < (int) XI_OPT_PASS_ID_COUNT; i++) {
        if (strcmp(xi_pass_names[i], name) == 0)
            return i;
    }
    return -1;
}

XR_FUNC const char *xi_pass_level_name(XiOptLevel level) {
    switch (level) {
        case XI_OPT_NONE:
            return "none";
        case XI_OPT_LIGHT:
            return "light";
        case XI_OPT_FULL:
            return "full";
        default:
            return NULL;
    }
}

XR_FUNC const char *xi_pass_pipeline_name(XiOptPipelineId pipeline) {
    switch (pipeline) {
        case XI_OPT_PIPELINE_VM:
            return "vm";
        case XI_OPT_PIPELINE_AOT:
            return "aot";
        default:
            return NULL;
    }
}

static bool xi_opt_level_by_name(const char *name, XiOptLevel *out) {
    XR_DCHECK(out != NULL, "level output is required");
    if (!name)
        return false;
    if (strcmp(name, "none") == 0) {
        *out = XI_OPT_NONE;
        return true;
    }
    if (strcmp(name, "light") == 0) {
        *out = XI_OPT_LIGHT;
        return true;
    }
    if (strcmp(name, "full") == 0) {
        *out = XI_OPT_FULL;
        return true;
    }
    return false;
}

static bool xi_opt_pipeline_by_name(const char *name, XiOptPipelineId *out) {
    XR_DCHECK(out != NULL, "pipeline output is required");
    if (!name)
        return false;
    if (strcmp(name, "vm") == 0) {
        *out = XI_OPT_PIPELINE_VM;
        return true;
    }
    if (strcmp(name, "aot") == 0) {
        *out = XI_OPT_PIPELINE_AOT;
        return true;
    }
    return false;
}

/* ========== Built-in default ========== */

/* Passes withheld from the native backend at the full level.
 *
 * loop_split was withheld here because it answered programs incorrectly: it
 * decided an early exit was dead by evaluating its condition at the induction
 * variable's first value, which says nothing about later iterations. That is
 * repaired at the source. The pass now requires the interval the loop header
 * establishes to imply the exit is never taken, and over the 52 differential
 * cases under tests/diff/cases/semantics/optimizer enabling it changes no
 * answer at all: not on the VM at the full level and not on the native
 * backend. It is released.
 *
 * The proof it now demands is strong enough that real loop bounds rarely
 * satisfy it, so the pass currently rewrites nothing. That makes it correct
 * and worthless rather than correct and useful, which is a question about
 * whether to keep the pass, not about whether to withhold it.
 *
 * ifconv is deliberately NOT withheld. Its defect reaches the native backend
 * as a refusal, not as a wrong answer, and a loud refusal is the acceptable
 * failure; switching a pass off would still be the wrong repair -- the defect
 * is being fixed in if-conversion itself.
 *
 * ivsr is withheld for an older and separate reason carried over unchanged:
 * its rewrite is not honoured through representation selection.
 *
 * This set is containment, not a repair. Take a pass out of it once enabling
 * that pass introduces no wrong answer and no new refusal over the corpus
 * above, on the VM at the full level and on the native backend. This is the
 * only place a pass is withheld. */
#define XI_PASS_AOT_WITHHELD_PASSES (XI_OPT_DISABLE_IVSR)

XR_FUNC XiOptPolicy xi_pass_policy_builtin_default(void) {
    XiOptPolicy policy;
    memset(&policy, 0, sizeof(policy));
    /* The VM stays at the light level. Every pass above it that is known to
     * be defective is defective on both pipelines, and the VM is the lane
     * that runs a program without a native toolchain. */
    policy.pipelines[XI_OPT_PIPELINE_VM].level = XI_OPT_LIGHT;
    policy.pipelines[XI_OPT_PIPELINE_VM].disabled = XI_OPT_DISABLE_NONE;
    policy.pipelines[XI_OPT_PIPELINE_AOT].level = XI_OPT_FULL;
    policy.pipelines[XI_OPT_PIPELINE_AOT].disabled = XI_PASS_AOT_WITHHELD_PASSES;
    return policy;
}

XR_FUNC bool xi_pass_policy_is_builtin_default(const XiOptPolicy *policy) {
    XiOptPolicy builtin;
    if (!policy)
        return false;
    builtin = xi_pass_policy_builtin_default();
    for (int i = 0; i < (int) XI_OPT_PIPELINE_COUNT; i++) {
        if (policy->pipelines[i].level != builtin.pipelines[i].level)
            return false;
        if (policy->pipelines[i].disabled != builtin.pipelines[i].disabled)
            return false;
    }
    return true;
}

/* ========== Spec parsing ========== */

static void xi_pass_policy_set_err(char *err, size_t err_size, const char *fmt, ...) {
    va_list args;
    if (!err || err_size == 0)
        return;
    va_start(args, fmt);
    (void) vsnprintf(err, err_size, fmt, args);
    va_end(args);
}

/* Parse one "pipeline=level(-pass)*" entry into `policy`. */
static bool xi_pass_policy_apply_entry(XiOptPolicy *policy, char *entry, char *err,
                                       size_t err_size) {
    char *eq;
    char *level_text;
    char *dash;
    XiOptPipelineId pipeline;
    XiOptLevel level;
    XiOptDisableMask disabled = XI_OPT_DISABLE_NONE;

    XR_DCHECK(policy != NULL, "policy is required");
    XR_DCHECK(entry != NULL, "entry is required");

    eq = strchr(entry, '=');
    if (!eq) {
        xi_pass_policy_set_err(err, err_size,
                               "entry '%s' has no '=': expected pipeline=level[-pass...]", entry);
        return false;
    }
    *eq = '\0';
    if (!xi_opt_pipeline_by_name(entry, &pipeline)) {
        xi_pass_policy_set_err(err, err_size, "unknown pipeline '%s': expected vm or aot", entry);
        return false;
    }

    level_text = eq + 1;
    dash = strchr(level_text, '-');
    if (dash)
        *dash = '\0';
    if (!xi_opt_level_by_name(level_text, &level)) {
        xi_pass_policy_set_err(err, err_size,
                               "unknown optimization level '%s': expected none, light or full",
                               level_text);
        return false;
    }

    while (dash) {
        char *pass_name = dash + 1;
        int pass_id;
        dash = strchr(pass_name, '-');
        if (dash)
            *dash = '\0';
        if (!pass_name[0]) {
            xi_pass_policy_set_err(err, err_size, "empty pass name after '-'");
            return false;
        }
        pass_id = xi_pass_id_by_name(pass_name);
        if (pass_id < 0) {
            xi_pass_policy_set_err(err, err_size, "unknown pass '%s'", pass_name);
            return false;
        }
        /* A required pass is structural: the driver runs it whatever the mask
         * says. Accepting its name would hand back a policy that renders as
         * withholding it while the pass keeps running, and the caller would
         * read every later difference as that pass' doing. */
        if (xi_pass_id_is_required(pass_id)) {
            xi_pass_policy_set_err(err, err_size,
                                   "pass '%s' is required and cannot be withheld", pass_name);
            return false;
        }
        disabled |= XI_OPT_DISABLE_BIT(pass_id);
    }

    policy->pipelines[pipeline].level = level;
    policy->pipelines[pipeline].disabled = disabled;
    return true;
}

XR_FUNC bool xi_pass_policy_apply_spec(XiOptPolicy *policy, const char *spec, char *err,
                                       size_t err_size) {
    XiOptPolicy staged;
    size_t len;
    char *buf;
    char *save = NULL;
    char *tok;
    bool ok = true;
    bool saw_entry = false;

    if (!policy) {
        xi_pass_policy_set_err(err, err_size, "no policy to apply the spec to");
        return false;
    }
    if (!spec) {
        xi_pass_policy_set_err(err, err_size, "no optimizer spec given");
        return false;
    }

    /* The whole entry list is rewritten in place while parsing, so it is
     * copied first and the caller's policy is only committed on success. */
    staged = *policy;
    len = strlen(spec);
    buf = (char *) xr_malloc(len + 1);
    if (!buf) {
        xi_pass_policy_set_err(err, err_size, "out of memory parsing optimizer spec");
        return false;
    }
    memcpy(buf, spec, len + 1);

    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (!tok[0])
            continue;
        saw_entry = true;
        if (!xi_pass_policy_apply_entry(&staged, tok, err, err_size)) {
            ok = false;
            break;
        }
    }
    xr_free(buf);

    if (!ok)
        return false;
    if (!saw_entry) {
        xi_pass_policy_set_err(err, err_size,
                               "optimizer spec is empty: expected pipeline=level[-pass...]");
        return false;
    }
    *policy = staged;
    return true;
}

XR_FUNC bool xi_pass_policy_render(const XiOptPolicy *policy, char *buf, size_t buf_size) {
    size_t used = 0;
    if (!policy || !buf || buf_size == 0)
        return false;
    buf[0] = '\0';
    for (int p = 0; p < (int) XI_OPT_PIPELINE_COUNT; p++) {
        const XiOptPipelinePolicy *entry = &policy->pipelines[p];
        const char *pipeline_name = xi_pass_pipeline_name((XiOptPipelineId) p);
        const char *level_name = xi_pass_level_name(entry->level);
        int written;
        if (!pipeline_name || !level_name)
            return false;
        written = snprintf(buf + used, buf_size - used, "%s%s=%s", used ? "," : "", pipeline_name,
                           level_name);
        if (written < 0 || (size_t) written >= buf_size - used)
            return false;
        used += (size_t) written;
        for (int i = 0; i < (int) XI_OPT_PASS_ID_COUNT; i++) {
            if (!(entry->disabled & XI_OPT_DISABLE_BIT(i)))
                continue;
            written = snprintf(buf + used, buf_size - used, "-%s", xi_pass_names[i]);
            if (written < 0 || (size_t) written >= buf_size - used)
                return false;
            used += (size_t) written;
        }
    }
    return true;
}

/* ========== Session policy ========== */

/* One policy per process. A compiler invocation is one compilation session,
 * so this is the session scope: every module the session compiles reads the
 * same entry, and cross-module inlining cannot mix two configurations. */
static XiOptPolicy xi_pass_session;
static bool xi_pass_session_sealed = false;
static xr_once_t xi_pass_session_once = XR_ONCE_INITIALIZER;

static void xi_pass_session_resolve_environment(void) {
    const char *spec = getenv("XRAY_XI_OPT");
    char err[256];
    xi_pass_session = xi_pass_policy_builtin_default();
    if (!spec || !spec[0])
        return;
    err[0] = '\0';
    /* A malformed request must not silently select a level nobody asked for:
     * the session stops here rather than compiling under a guessed policy. */
    XR_CHECK_FMT(xi_pass_policy_apply_spec(&xi_pass_session, spec, err, sizeof(err)),
                 "XRAY_XI_OPT is invalid: %s", err);
}

XR_FUNC const XiOptPolicy *xi_pass_session_policy(void) {
    xr_once_call(&xi_pass_session_once, xi_pass_session_resolve_environment);
    return &xi_pass_session;
}

XR_FUNC XiOptPipelinePolicy xi_pass_session_pipeline_policy(XiOptPipelineId pipeline) {
    const XiOptPolicy *policy = xi_pass_session_policy();
    XR_CHECK(pipeline >= 0 && pipeline < XI_OPT_PIPELINE_COUNT,
             "optimizer policy addressed with an unknown pipeline");
    /* Reading is what fixes the policy for the rest of the session. */
    xi_pass_session_sealed = true;
    return policy->pipelines[pipeline];
}

XR_FUNC bool xi_pass_session_policy_apply_spec(const char *spec, const char *origin, char *err,
                                               size_t err_size) {
    XiOptPolicy staged;
    const char *label = (origin && origin[0]) ? origin : "optimizer spec";

    (void) xi_pass_session_policy(); /* resolve the environment layer first */
    if (xi_pass_session_sealed) {
        xi_pass_policy_set_err(err, err_size,
                               "%s cannot change the optimizer policy: a pipeline in this session "
                               "has already been configured from it",
                               label);
        return false;
    }
    staged = xi_pass_session;
    if (!xi_pass_policy_apply_spec(&staged, spec, err, err_size))
        return false;
    xi_pass_session = staged;
    return true;
}

XR_FUNC void xi_pass_session_policy_reset_for_testing(void) {
    (void) xi_pass_session_policy();
    xi_pass_session = xi_pass_policy_builtin_default();
    xi_pass_session_sealed = false;
}
