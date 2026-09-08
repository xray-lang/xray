/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_cleanup_integration.c - Cleanup validation and edit evidence contracts
 */

#include "../test_framework.h"
#include "ir/xi_cleanup.h"
#include "ir/xi_edit.h"
#include "ir/xi_evidence.h"
#include "ir/xi_verify.h"
#include "runtime/value/xtype.h"

static XrType cleanup_unit = {.kind = XR_KIND_UNIT, .id = 1u, .frozen = true};
static XrType cleanup_int = {.kind = XR_KIND_INT, .id = 2u, .frozen = true};

typedef struct CleanupEditFixture {
    XiFunc *function;
    XiValue *body;
    XiValue *enter[2];
    XiValue *leave[2];
} CleanupEditFixture;

static bool attach_frontier(CleanupEditFixture *fixture) {
    char error[192];
    for (uint32_t index = 0u; index < 2u; ++index) {
        XiCleanupBoundary boundary = {
            .enter = fixture->enter[index],
            .leave = fixture->leave[index],
            .remaining = index == 0u ? fixture->enter[1] : NULL,
            .frontier = fixture->enter[0],
            .rank = 2u - index,
            .kind = XI_CLEANUP_BOUNDARY_CLOSED,
        };
        if (!xi_cleanup_boundary_attach(fixture->function, &boundary, error, sizeof(error)))
            return false;
    }
    return true;
}

static CleanupEditFixture make_fixture(bool attach) {
    CleanupEditFixture fixture = {.function = xi_func_new("cleanup_edit", &cleanup_unit)};
    if (!fixture.function)
        return fixture;
    XiBlock *block = xi_block_new(fixture.function);
    if (!block)
        goto fail;
    block->sealed = true;
    fixture.body = xi_const_int(fixture.function, block, 42, &cleanup_int);
    if (!fixture.body)
        goto fail;
    for (uint32_t index = 0u; index < 2u; ++index) {
        fixture.enter[index] =
            xi_value_new(fixture.function, block, XI_CLEANUP_ENTER, &cleanup_unit, 0u);
        fixture.leave[index] =
            xi_value_new(fixture.function, block, XI_CLEANUP_LEAVE, &cleanup_unit, 0u);
        if (!fixture.enter[index] || !fixture.leave[index])
            goto fail;
    }
    xi_block_set_return(block, NULL);
    if (attach && !attach_frontier(&fixture))
        goto fail;
    return fixture;

fail:
    xi_func_free(fixture.function);
    return (CleanupEditFixture) {0};
}

static bool cleanup_rejected(const XiFunc *function) {
    char error[192] = {0};
    return !xi_verify(function, error, sizeof(error)) && strstr(error, "cleanup") != NULL;
}

/* Both shapes are valid, but partitioning one frontier into two changes the
 * static cleanup obligations without touching a CFG successor or opcode. */
static void split_frontier(CleanupEditFixture *fixture) {
    fixture->enter[0]->cleanup_boundary->remaining = NULL;
    fixture->enter[0]->cleanup_boundary->rank = 1u;
    fixture->enter[1]->cleanup_boundary->frontier = fixture->enter[1];
}

TEST(normal_verifier_accepts_complete_frontiers_and_absence_of_markers) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    char error[192];
    bool valid = xi_verify(fixture.function, error, sizeof(error));
    split_frontier(&fixture);
    valid = valid && xi_verify(fixture.function, error, sizeof(error));
    /* Removing all markers leaves no live cleanup obligation. Arena records
     * are not a second registry and must not resurrect dead instructions. */
    fixture.function->entry->nvalues = 1u;
    valid = valid && xi_verify(fixture.function, error, sizeof(error));
    xi_func_free(fixture.function);
    ASSERT(valid);
}

TEST(every_stage_rejects_each_missing_live_marker_record) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    bool rejected = true;
    for (uint32_t index = 0u; index < 4u; ++index) {
        XiValue *marker = index < 2u ? fixture.enter[index] : fixture.leave[index - 2u];
        XiCleanupBoundary *saved = marker->cleanup_boundary;
        marker->cleanup_boundary = NULL;
        rejected = cleanup_rejected(fixture.function) && rejected;
        for (int stage = XI_STAGE_RAW; stage <= XI_STAGE_BACKEND; ++stage) {
            char error[192] = {0};
            bool accepted =
                xi_verify_stage(fixture.function, (XiStage) stage, error, sizeof(error));
            rejected = !accepted && strstr(error, "cleanup") != NULL && rejected;
        }
        marker->cleanup_boundary = saved;
    }
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(minimum_diagnostic_capacity_never_writes_past_the_buffer) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    fixture.enter[0]->cleanup_boundary = NULL;
    struct {
        char buffer[1];
        char guard;
    } output = {{'X'}, 'Q'};
    bool rejected = !xi_verify(fixture.function, output.buffer, sizeof(output.buffer)) &&
                    output.buffer[0] == '\0' && output.guard == 'Q';
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(every_boundary_field_changes_semantic_and_cfg_fingerprints) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    XiCleanupBoundary *boundary = fixture.enter[0]->cleanup_boundary;
    XiCleanupBoundary saved = *boundary;
    XiEditFingerprint before = xi_edit_fingerprint(fixture.function);
    bool covered = true;
    for (uint32_t mutation = 0u; mutation < 9u; ++mutation) {
        *boundary = saved;
        switch (mutation) {
            case 0u:
                boundary->enter = fixture.enter[1];
                break;
            case 1u:
                boundary->leave = fixture.leave[1];
                break;
            case 2u:
                boundary->remaining = NULL;
                break;
            case 3u:
                boundary->frontier = fixture.enter[1];
                break;
            case 4u:
                boundary->rank = 3u;
                break;
            case 5u:
                boundary->kind = XI_CLEANUP_BOUNDARY_FATAL;
                break;
            default:
                boundary->reserved[mutation - 6u] = 1u;
                break;
        }
        XiEditFingerprint after = xi_edit_fingerprint(fixture.function);
        covered = before.cfg != after.cfg && before.values != after.values &&
                  cleanup_rejected(fixture.function) && covered;
    }
    *boundary = saved;
    xi_func_free(fixture.function);
    ASSERT(covered);
}

TEST(fingerprints_reject_storage_and_member_impostors_without_dereferencing_them) {
    CleanupEditFixture fixture = make_fixture(true);
    CleanupEditFixture foreign = make_fixture(true);
    ASSERT(fixture.function && foreign.function);
    XiCleanupBoundary *owned = fixture.enter[0]->cleanup_boundary;
    XiEditFingerprint before = xi_edit_fingerprint(fixture.function);
    bool rejected = true;
    XiCleanupBoundary *records[] = {NULL, (XiCleanupBoundary *) (uintptr_t) 1u,
                                    foreign.enter[0]->cleanup_boundary};
    for (size_t index = 0u; index < sizeof(records) / sizeof(records[0]); ++index) {
        fixture.enter[0]->cleanup_boundary = records[index];
        XiEditFingerprint after = xi_edit_fingerprint(fixture.function);
        rejected = before.cfg != after.cfg && before.values != after.values &&
                   cleanup_rejected(fixture.function) && rejected;
    }
    fixture.enter[0]->cleanup_boundary = owned;
    XiValue *saved = owned->remaining;
    XiValue *members[] = {(XiValue *) (uintptr_t) 1u, foreign.enter[1]};
    /* The foreign marker deliberately has the same ID as the local marker. */
    rejected = foreign.enter[1]->id == saved->id && rejected;
    for (size_t index = 0u; index < sizeof(members) / sizeof(members[0]); ++index) {
        owned->remaining = members[index];
        XiEditFingerprint after = xi_edit_fingerprint(fixture.function);
        rejected = before.cfg != after.cfg && before.values != after.values &&
                   cleanup_rejected(fixture.function) && rejected;
    }
    owned->remaining = saved;
    xi_func_free(foreign.function);
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(record_relocation_preserves_meaning_but_split_pair_identity_does_not) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    XiCleanupBoundary *replacement =
        xi_func_arena_alloc(fixture.function, (uint32_t) sizeof(*replacement));
    ASSERT(replacement);
    *replacement = *fixture.enter[0]->cleanup_boundary;
    XiEditSession edit;
    ASSERT(xi_edit_begin(&edit, fixture.function));
    fixture.enter[0]->cleanup_boundary = replacement;
    XiEditFingerprint split = xi_edit_fingerprint(fixture.function);
    bool valid = split.cfg != edit.before.cfg && split.values != edit.before.values &&
                 cleanup_rejected(fixture.function);
    fixture.leave[0]->cleanup_boundary = replacement;
    XiEditFingerprint relocated = xi_edit_fingerprint(fixture.function);
    XiPassOutcome outcome;
    char error[192];
    valid = valid && relocated.cfg == edit.before.cfg && relocated.values == edit.before.values &&
            xi_verify(fixture.function, error, sizeof(error)) &&
            xi_edit_finish(&edit, xi_pass_no_change(), 0u, 0u, &outcome, error, sizeof(error)) &&
            !outcome.revision_delta.ir_changed && !outcome.revision_delta.cfg_changed;
    xi_func_free(fixture.function);
    ASSERT(valid);
}

TEST(unreported_cleanup_rewrite_and_values_only_report_fail_closed) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    XiEditSession edit;
    ASSERT(xi_edit_begin(&edit, fixture.function));
    split_frontier(&fixture);
    XiPassOutcome outcome;
    char error[192];
    XiPassChange change = xi_pass_no_change();
    bool rejected = !xi_edit_finish(&edit, change, 0u, 0u, &outcome, error, sizeof(error)) &&
                    strstr(error, "cfg_changed") != NULL;
    change.values_changed = true;
    rejected = !xi_edit_finish(&edit, change, 0u, 0u, &outcome, error, sizeof(error)) &&
               strstr(error, "cfg_changed") != NULL && rejected;
    rejected = fixture.function->ir_revision == edit.ir_revision &&
               fixture.function->cfg_version == edit.cfg_revision && rejected;
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(cleanup_rewrite_invalidates_control_owner_and_effect_evidence) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    bool published = true;
    for (uint32_t bit = 1u; bit <= XI_EVD_MEMSSA; bit <<= 1u) {
        published = xi_evidence_publish(fixture.function, (XiEvidenceDomain) bit,
                                        xi_evidence_subject_function(), XI_PROOF_PROVEN,
                                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST, 0u,
                                        NULL) != NULL &&
                    published;
    }
    ASSERT(published);
    XiEditSession edit;
    ASSERT(xi_edit_begin(&edit, fixture.function));
    split_frontier(&fixture);
    XiPassChange change = xi_pass_no_change();
    change.cfg_changed = true;
    XiPassOutcome outcome;
    char error[192];
    const XiEvidenceDomainMask invalidated = XI_EVD_RANGE | XI_EVD_ESCAPE | XI_EVD_OWNERSHIP |
                                             XI_EVD_LIFETIME | XI_EVD_MEMSSA | XI_EVD_EFFECT |
                                             XI_EVD_NOALLOC;
    bool valid = xi_verify(fixture.function, error, sizeof(error)) &&
                 xi_edit_finish(&edit, change, 0u, 0u, &outcome, error, sizeof(error)) &&
                 outcome.invalidates == invalidated && outcome.revision_delta.ir_changed &&
                 outcome.revision_delta.cfg_changed;
    for (uint32_t bit = 1u; bit <= XI_EVD_MEMSSA; bit <<= 1u) {
        bool current = xi_evidence_domain_is_current(fixture.function, (XiEvidenceDomain) bit);
        valid = current == ((invalidated & bit) == 0u) && valid;
    }
    xi_func_free(fixture.function);
    ASSERT(valid);
}

TEST(noncleanup_body_edit_does_not_change_cfg_fingerprint) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    XiEditSession edit;
    ASSERT(xi_edit_begin(&edit, fixture.function));
    fixture.body->aux_int = 43;
    XiEditFingerprint after = xi_edit_fingerprint(fixture.function);
    XiPassChange change = xi_pass_no_change();
    change.values_changed = true;
    XiPassOutcome outcome;
    char error[192];
    bool valid = after.cfg == edit.before.cfg && after.values != edit.before.values &&
                 xi_edit_finish(&edit, change, 0u, 0u, &outcome, error, sizeof(error)) &&
                 !outcome.revision_delta.cfg_changed;
    xi_func_free(fixture.function);
    ASSERT(valid);
}

TEST(attaching_or_erasing_cleanup_identity_requires_a_cfg_report) {
    CleanupEditFixture fixture = make_fixture(false);
    ASSERT(fixture.function);
    XiEditSession edit;
    ASSERT(xi_edit_begin(&edit, fixture.function));
    ASSERT(attach_frontier(&fixture));
    XiPassOutcome outcome;
    char error[192];
    bool rejected =
        !xi_edit_finish(&edit, xi_pass_no_change(), 0u, 0u, &outcome, error, sizeof(error)) &&
        strstr(error, "cfg_changed") != NULL;
    ASSERT(xi_edit_begin(&edit, fixture.function));
    fixture.enter[0]->cleanup_boundary = NULL;
    fixture.leave[0]->cleanup_boundary = NULL;
    rejected =
        !xi_edit_finish(&edit, xi_pass_no_change(), 0u, 0u, &outcome, error, sizeof(error)) &&
        strstr(error, "cfg_changed") != NULL && rejected;
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(ordinary_instructions_cannot_silently_acquire_cleanup_identity) {
    CleanupEditFixture fixture = make_fixture(true);
    ASSERT(fixture.function);
    XiEditFingerprint before = xi_edit_fingerprint(fixture.function);
    fixture.body->cleanup_boundary = fixture.enter[0]->cleanup_boundary;
    XiEditFingerprint after = xi_edit_fingerprint(fixture.function);
    bool rejected = before.cfg != after.cfg && before.values != after.values &&
                    cleanup_rejected(fixture.function);
    fixture.body->cleanup_boundary = NULL;
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Xi cleanup validation and edit evidence");
RUN_TEST(normal_verifier_accepts_complete_frontiers_and_absence_of_markers);
RUN_TEST(every_stage_rejects_each_missing_live_marker_record);
RUN_TEST(minimum_diagnostic_capacity_never_writes_past_the_buffer);
RUN_TEST(every_boundary_field_changes_semantic_and_cfg_fingerprints);
RUN_TEST(fingerprints_reject_storage_and_member_impostors_without_dereferencing_them);
RUN_TEST(record_relocation_preserves_meaning_but_split_pair_identity_does_not);
RUN_TEST(unreported_cleanup_rewrite_and_values_only_report_fail_closed);
RUN_TEST(cleanup_rewrite_invalidates_control_owner_and_effect_evidence);
RUN_TEST(noncleanup_body_edit_does_not_change_cfg_fingerprint);
RUN_TEST(attaching_or_erasing_cleanup_identity_requires_a_cfg_report);
RUN_TEST(ordinary_instructions_cannot_silently_acquire_cleanup_identity);
TEST_MAIN_END()
