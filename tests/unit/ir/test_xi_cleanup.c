/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_cleanup.c - Independent cleanup identity and clone rejection tests
 */

#include "../test_framework.h"
#include "ir/xi_cleanup.h"
#include "runtime/value/xtype.h"

#include <stdint.h>
#include <string.h>

enum {
    CLEANUP_TEST_CAPACITY = 4,
    CLEANUP_TEST_MAP_CAPACITY = 8
};

static XrType cleanup_unit = {.kind = XR_KIND_UNIT, .id = 1u, .frozen = true};

typedef struct CleanupFixture {
    XiFunc *function;
    XiValue *enter[CLEANUP_TEST_CAPACITY];
    XiValue *leave[CLEANUP_TEST_CAPACITY];
    uint32_t count;
} CleanupFixture;

/* Create every identity before attachment, including forward references. */
static bool cleanup_fixture_append(CleanupFixture *fixture, XiFunc *function, uint32_t count,
                                   bool fatal_last) {
    *fixture = (CleanupFixture) {.function = function, .count = count};
    if (!function || count == 0u || count > CLEANUP_TEST_CAPACITY)
        return false;
    XiBlock *block = xi_block_new(function);
    if (!block)
        return false;
    block->sealed = true;
    for (uint32_t index = 0u; index < count; ++index) {
        fixture->enter[index] = xi_value_new(function, block, XI_CLEANUP_ENTER, &cleanup_unit, 0u);
        if (!fixture->enter[index])
            return false;
        fixture->enter[index]->flags |= XI_FLAG_SIDE_EFFECT;
        if (fatal_last && index + 1u == count)
            continue;
        fixture->leave[index] = xi_value_new(function, block, XI_CLEANUP_LEAVE, &cleanup_unit, 0u);
        if (!fixture->leave[index])
            return false;
        fixture->leave[index]->flags |= XI_FLAG_SIDE_EFFECT;
    }
    xi_block_set_return(block, NULL);
    return true;
}

static XiCleanupBoundary cleanup_fixture_row(const CleanupFixture *fixture, uint32_t index) {
    return (XiCleanupBoundary) {
        .enter = fixture->enter[index],
        .leave = fixture->leave[index],
        .remaining = index + 1u < fixture->count ? fixture->enter[index + 1u] : NULL,
        .frontier = fixture->enter[0],
        .rank = fixture->count - index,
        .kind = fixture->leave[index] ? XI_CLEANUP_BOUNDARY_CLOSED : XI_CLEANUP_BOUNDARY_FATAL,
    };
}

static bool cleanup_fixture_attach(CleanupFixture *fixture) {
    char error[192];
    for (uint32_t remaining = fixture->count; remaining > 0u; --remaining) {
        XiCleanupBoundary row = cleanup_fixture_row(fixture, remaining - 1u);
        if (!xi_cleanup_boundary_attach(fixture->function, &row, error, sizeof(error)))
            return false;
        /* The receiving arena, not this temporary input, owns the record. */
        memset(&row, 0, sizeof(row));
    }
    return xi_cleanup_verify(fixture->function, error, sizeof(error));
}

static CleanupFixture cleanup_fixture_new(uint32_t count, bool fatal_last, bool attach) {
    CleanupFixture fixture = {0};
    XiFunc *function = xi_func_new("cleanup_identity", &cleanup_unit);
    if (!cleanup_fixture_append(&fixture, function, count, fatal_last) ||
        (attach && !cleanup_fixture_attach(&fixture))) {
        xi_func_free(function);
        return (CleanupFixture) {0};
    }
    return fixture;
}

static bool cleanup_rejected(const CleanupFixture *fixture) {
    char error[192] = {0};
    return !xi_cleanup_verify(fixture->function, error, sizeof(error)) && error[0] != '\0';
}

static bool cleanup_unattached(const CleanupFixture *fixture) {
    for (uint32_t index = 0u; index < fixture->count; ++index) {
        if (fixture->enter[index]->cleanup_boundary ||
            (fixture->leave[index] && fixture->leave[index]->cleanup_boundary))
            return false;
    }
    return true;
}

static uint32_t cleanup_fixture_map(const CleanupFixture *source, const CleanupFixture *target,
                                    XiCleanupValueMapping *values) {
    uint32_t count = 0u;
    /* Reverse order deliberately prevents one-pass forward-pointer copying. */
    for (uint32_t remaining = source->count; remaining > 0u; --remaining) {
        uint32_t index = remaining - 1u;
        if (source->leave[index])
            values[count++] = (XiCleanupValueMapping) {source->leave[index], target->leave[index]};
        values[count++] = (XiCleanupValueMapping) {source->enter[index], target->enter[index]};
    }
    return count;
}

TEST(complete_frontier_owns_copied_pairs) {
    CleanupFixture fixture = cleanup_fixture_new(3u, false, true);
    ASSERT(fixture.function != NULL);
    bool valid = true;
    for (uint32_t index = 0u; index < fixture.count; ++index) {
        const XiCleanupBoundary *row = fixture.enter[index]->cleanup_boundary;
        valid = valid && row && row == fixture.leave[index]->cleanup_boundary &&
                xi_func_arena_contains(fixture.function, row, (uint32_t) sizeof(*row)) &&
                row->enter == fixture.enter[index] && row->leave == fixture.leave[index] &&
                row->frontier == fixture.enter[0] && row->rank == fixture.count - index &&
                row->remaining == (index + 1u < fixture.count ? fixture.enter[index + 1u] : NULL);
    }
    char error[192] = "stale diagnostic";
    valid = valid && xi_cleanup_verify(fixture.function, error, sizeof(error)) && error[0] == '\0';
    xi_func_free(fixture.function);
    ASSERT(valid);
}

TEST(fatal_and_closed_completions_are_distinct) {
    CleanupFixture fatal = cleanup_fixture_new(1u, true, true);
    CleanupFixture closed = cleanup_fixture_new(1u, false, true);
    ASSERT(fatal.function && closed.function);
    fatal.enter[0]->cleanup_boundary->kind = XI_CLEANUP_BOUNDARY_CLOSED;
    bool missing_leave_rejected = cleanup_rejected(&fatal);
    fatal.enter[0]->cleanup_boundary->kind = XI_CLEANUP_BOUNDARY_FATAL;
    closed.enter[0]->cleanup_boundary->kind = XI_CLEANUP_BOUNDARY_FATAL;
    bool spurious_leave_rejected = cleanup_rejected(&closed);
    closed.enter[0]->cleanup_boundary->kind = XI_CLEANUP_BOUNDARY_CLOSED;
    char error[192];
    bool restored = xi_cleanup_verify(fatal.function, error, sizeof(error)) &&
                    xi_cleanup_verify(closed.function, error, sizeof(error));
    xi_func_free(fatal.function);
    xi_func_free(closed.function);
    ASSERT(missing_leave_rejected && spurious_leave_rejected && restored);
}

TEST(unowned_and_unaligned_records_are_rejected_before_read) {
    CleanupFixture fixture = cleanup_fixture_new(1u, false, true);
    CleanupFixture foreign = cleanup_fixture_new(1u, false, true);
    ASSERT(fixture.function && foreign.function);
    XiCleanupBoundary *owned = fixture.enter[0]->cleanup_boundary;
    XiCleanupBoundary stack_copy = *owned;
    XiCleanupBoundary *invalid[] = {
        (XiCleanupBoundary *) (uintptr_t) 1u,
        foreign.enter[0]->cleanup_boundary,
        &stack_copy,
        (XiCleanupBoundary *) ((unsigned char *) owned + 1u),
    };
    bool rejected = true;
    for (uint32_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        fixture.enter[0]->cleanup_boundary = invalid[index];
        rejected = cleanup_rejected(&fixture) && rejected;
    }
    fixture.enter[0]->cleanup_boundary = owned;
    xi_func_free(fixture.function);
    xi_func_free(foreign.function);
    ASSERT(rejected);
}

TEST(poisoned_member_identities_are_rejected_before_read) {
    CleanupFixture fixture = cleanup_fixture_new(2u, false, true);
    ASSERT(fixture.function != NULL);
    XiCleanupBoundary *row = fixture.enter[0]->cleanup_boundary;
    XiCleanupBoundary saved = *row;
    XiValue *poison = (XiValue *) (uintptr_t) 1u;
    bool rejected = true;
    row->enter = poison;
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    row->leave = poison;
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    row->remaining = poison;
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    row->frontier = poison;
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    /* Arena membership alone cannot prove a removed instruction is still live. */
    XiBlock *block = fixture.enter[1]->block;
    for (uint32_t index = 0u; index < block->nvalues; ++index) {
        if (block->values[index] != fixture.enter[1])
            continue;
        block->values[index] = NULL;
        rejected = cleanup_rejected(&fixture) && rejected;
        block->values[index] = fixture.enter[1];
        break;
    }
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(pair_rank_and_frontier_corruption_is_rejected) {
    CleanupFixture fixture = cleanup_fixture_new(3u, false, true);
    ASSERT(fixture.function != NULL);
    XiCleanupBoundary *head = fixture.enter[0]->cleanup_boundary;
    XiCleanupBoundary *middle = fixture.enter[1]->cleanup_boundary;
    XiCleanupBoundary *tail = fixture.enter[2]->cleanup_boundary;
    XiCleanupBoundary saved_head = *head;
    XiCleanupBoundary saved_middle = *middle;
    XiCleanupBoundary saved_tail = *tail;
    bool rejected = true;
    fixture.leave[0]->cleanup_boundary = middle;
    rejected = cleanup_rejected(&fixture) && rejected;
    fixture.leave[0]->cleanup_boundary = head;
    head->rank = 0u;
    rejected = cleanup_rejected(&fixture) && rejected;
    head->rank = UINT32_MAX;
    rejected = cleanup_rejected(&fixture) && rejected;
    *head = saved_head;
    head->reserved[1] = 1u;
    rejected = cleanup_rejected(&fixture) && rejected;
    *head = saved_head;
    head->remaining = fixture.enter[2];
    rejected = cleanup_rejected(&fixture) && rejected;
    *head = saved_head;
    middle->frontier = middle->enter;
    rejected = cleanup_rejected(&fixture) && rejected;
    *middle = saved_middle;
    tail->rank = 2u;
    tail->remaining = head->enter;
    rejected = cleanup_rejected(&fixture) && rejected;
    *tail = saved_tail;
    head->frontier = middle->enter;
    rejected = cleanup_rejected(&fixture) && rejected;
    *head = saved_head;
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(cross_function_and_cross_frontier_relations_are_rejected) {
    CleanupFixture fixture = cleanup_fixture_new(2u, false, true);
    CleanupFixture foreign = cleanup_fixture_new(1u, false, true);
    ASSERT(fixture.function && foreign.function);
    CleanupFixture other = {0};
    bool created = cleanup_fixture_append(&other, fixture.function, 1u, false) &&
                   cleanup_fixture_attach(&other);
    if (!created) {
        xi_func_free(fixture.function);
        xi_func_free(foreign.function);
        ASSERT(created);
    }
    XiCleanupBoundary *row = fixture.enter[0]->cleanup_boundary;
    XiCleanupBoundary saved = *row;
    row->remaining = foreign.enter[0];
    bool rejected = cleanup_rejected(&fixture);
    *row = saved;
    row->leave = foreign.leave[0];
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    row->frontier = foreign.enter[0];
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    row->remaining = other.enter[0];
    rejected = cleanup_rejected(&fixture) && rejected;
    *row = saved;
    xi_func_free(fixture.function);
    xi_func_free(foreign.function);
    ASSERT(rejected);
}

TEST(nonmarkers_cannot_carry_boundary_identity) {
    CleanupFixture fixture = cleanup_fixture_new(1u, false, true);
    ASSERT(fixture.function != NULL);
    XiValue *ordinary =
        xi_value_new(fixture.function, fixture.function->entry, XI_CONST, &cleanup_unit, 0u);
    XiPhi *phi = xi_phi_new(fixture.function, fixture.function->entry, &cleanup_unit, 0u);
    ASSERT(ordinary && phi);
    ordinary->cleanup_boundary = fixture.enter[0]->cleanup_boundary;
    bool rejected = cleanup_rejected(&fixture);
    ordinary->cleanup_boundary = NULL;
    phi->value.cleanup_boundary = fixture.enter[0]->cleanup_boundary;
    rejected = cleanup_rejected(&fixture) && rejected;
    phi->value.cleanup_boundary = NULL;
    XiCleanupBoundary *saved = fixture.enter[0]->cleanup_boundary;
    fixture.enter[0]->cleanup_boundary = NULL;
    rejected = cleanup_rejected(&fixture) && rejected;
    fixture.enter[0]->cleanup_boundary = saved;
    xi_func_free(fixture.function);
    ASSERT(rejected);
}

TEST(attachment_refuses_replacement_and_invalid_pairs) {
    CleanupFixture fixture = cleanup_fixture_new(2u, false, false);
    ASSERT(fixture.function != NULL);
    char error[192];
    XiCleanupBoundary row = cleanup_fixture_row(&fixture, 0u);
    row.leave = fixture.enter[0];
    bool rejected = !xi_cleanup_boundary_attach(fixture.function, &row, error, sizeof(error)) &&
                    cleanup_unattached(&fixture);
    bool attached = cleanup_fixture_attach(&fixture);
    XiCleanupBoundary *owned = fixture.enter[0]->cleanup_boundary;
    row = cleanup_fixture_row(&fixture, 0u);
    rejected = !xi_cleanup_boundary_attach(fixture.function, &row, error, sizeof(error)) &&
               fixture.enter[0]->cleanup_boundary == owned && rejected;
    xi_func_free(fixture.function);
    ASSERT(attached && rejected);
}

TEST(complete_remap_survives_source_arena_destruction) {
    CleanupFixture source = cleanup_fixture_new(3u, false, true);
    CleanupFixture target = cleanup_fixture_new(3u, false, false);
    ASSERT(source.function && target.function);
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    XiCleanupRemap remap = {values, cleanup_fixture_map(&source, &target, values)};
    char error[192];
    bool copied = xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error));
    for (uint32_t index = 0u; index < source.count && copied; ++index) {
        const XiCleanupBoundary *row = target.enter[index]->cleanup_boundary;
        copied = row && row != source.enter[index]->cleanup_boundary &&
                 row->enter == target.enter[index] && row->leave == target.leave[index] &&
                 row->frontier == target.enter[0] &&
                 row->remaining == (index + 1u < target.count ? target.enter[index + 1u] : NULL);
    }
    xi_func_free(source.function);
    bool independent = copied && xi_cleanup_verify(target.function, error, sizeof(error));
    xi_func_free(target.function);
    ASSERT(independent);
}

TEST(same_function_clone_has_separate_frontier_occurrence) {
    CleanupFixture source = cleanup_fixture_new(2u, false, true);
    ASSERT(source.function != NULL);
    CleanupFixture target = {0};
    bool appended = cleanup_fixture_append(&target, source.function, 2u, false);
    if (!appended) {
        xi_func_free(source.function);
        ASSERT(appended);
    }
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    XiCleanupRemap remap = {values, cleanup_fixture_map(&source, &target, values)};
    char error[192];
    bool copied =
        xi_cleanup_remap(source.function, source.function, &remap, error, sizeof(error)) &&
        xi_cleanup_verify(source.function, error, sizeof(error)) &&
        target.enter[0]->cleanup_boundary->frontier == target.enter[0] &&
        source.enter[0]->cleanup_boundary->frontier == source.enter[0];
    xi_func_free(source.function);
    ASSERT(copied);
}

TEST(every_missing_marker_mapping_rejects_without_publication) {
    CleanupFixture source = cleanup_fixture_new(3u, false, true);
    ASSERT(source.function != NULL);
    bool rejected = true;
    for (uint32_t omitted = 0u; omitted < 6u; ++omitted) {
        CleanupFixture target = cleanup_fixture_new(3u, false, false);
        if (!target.function) {
            rejected = false;
            break;
        }
        XiCleanupValueMapping complete[CLEANUP_TEST_MAP_CAPACITY];
        XiCleanupValueMapping partial[CLEANUP_TEST_MAP_CAPACITY];
        uint32_t count = cleanup_fixture_map(&source, &target, complete);
        uint32_t kept = 0u;
        for (uint32_t index = 0u; index < count; ++index) {
            if (index != omitted)
                partial[kept++] = complete[index];
        }
        XiCleanupRemap remap = {partial, kept};
        char error[192];
        rejected =
            !xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
            error[0] != '\0' && cleanup_unattached(&target) && rejected;
        xi_func_free(target.function);
    }
    xi_func_free(source.function);
    ASSERT(rejected);
}

TEST(ambiguous_and_poisoned_mapping_rejects_without_publication) {
    CleanupFixture source = cleanup_fixture_new(2u, false, true);
    CleanupFixture target = cleanup_fixture_new(2u, false, false);
    ASSERT(source.function && target.function);
    XiCleanupValueMapping complete[CLEANUP_TEST_MAP_CAPACITY];
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    uint32_t count = cleanup_fixture_map(&source, &target, complete);
    XiCleanupRemap remap = {values, count};
    char error[192];
    bool rejected = true;
    for (uint32_t mutation = 0u; mutation < 5u; ++mutation) {
        memcpy(values, complete, count * sizeof(*values));
        switch (mutation) {
            case 0u:
                values[0].source = values[2].source;
                break;
            case 1u:
                values[0].target = values[2].target;
                break;
            case 2u:
                values[0].source = (XiValue *) (uintptr_t) 1u;
                break;
            case 3u:
                values[0].target = (XiValue *) (uintptr_t) 1u;
                break;
            default:
                values[0].target = values[1].target;
                break;
        }
        rejected =
            !xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
            error[0] != '\0' && cleanup_unattached(&target) && rejected;
    }
    xi_func_free(source.function);
    xi_func_free(target.function);
    ASSERT(rejected);
}

TEST(complete_pairs_without_complete_frontier_cannot_be_cloned) {
    CleanupFixture source = cleanup_fixture_new(3u, false, true);
    CleanupFixture target = cleanup_fixture_new(3u, false, false);
    ASSERT(source.function && target.function);
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    uint32_t count = cleanup_fixture_map(&source, &target, values);
    char error[192];
    bool rejected = true;
    for (uint32_t offset = 0u; offset < count; offset += 2u) {
        XiCleanupRemap pair = {values + offset, 2u};
        rejected =
            !xi_cleanup_remap(target.function, source.function, &pair, error, sizeof(error)) &&
            error[0] != '\0' && cleanup_unattached(&target) && rejected;
    }
    xi_func_free(source.function);
    xi_func_free(target.function);
    ASSERT(rejected);
}

TEST(target_order_and_occupied_records_cannot_be_overwritten) {
    CleanupFixture source = cleanup_fixture_new(2u, false, true);
    CleanupFixture target = cleanup_fixture_new(2u, false, false);
    ASSERT(source.function && target.function);
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    uint32_t count = cleanup_fixture_map(&source, &target, values);
    XiValue *saved_enter = values[1].target;
    XiValue *saved_leave = values[0].target;
    values[1].target = values[3].target;
    values[0].target = values[2].target;
    values[3].target = saved_enter;
    values[2].target = saved_leave;
    XiCleanupRemap remap = {values, count};
    char error[192];
    bool rejected =
        !xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
        cleanup_unattached(&target);
    (void) cleanup_fixture_map(&source, &target, values);
    bool attached = cleanup_fixture_attach(&target);
    XiCleanupBoundary *owned = target.enter[0]->cleanup_boundary;
    rejected = !xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
               target.enter[0]->cleanup_boundary == owned && rejected;
    xi_func_free(source.function);
    xi_func_free(target.function);
    ASSERT(attached && rejected);
}

TEST(independent_frontier_subset_does_not_require_other_occurrences) {
    CleanupFixture source = cleanup_fixture_new(2u, false, true);
    CleanupFixture target = cleanup_fixture_new(2u, false, false);
    ASSERT(source.function && target.function);
    CleanupFixture unrelated = {0};
    bool appended = cleanup_fixture_append(&unrelated, source.function, 1u, false) &&
                    cleanup_fixture_attach(&unrelated);
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    XiCleanupRemap remap = {values, cleanup_fixture_map(&source, &target, values)};
    char error[192];
    bool copied =
        appended &&
        xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
        xi_cleanup_verify(target.function, error, sizeof(error));
    xi_func_free(source.function);
    xi_func_free(target.function);
    ASSERT(copied);
}

TEST(fatal_remap_needs_no_synthetic_leave) {
    CleanupFixture source = cleanup_fixture_new(1u, true, true);
    CleanupFixture target = cleanup_fixture_new(1u, true, false);
    ASSERT(source.function && target.function);
    XiCleanupValueMapping value = {source.enter[0], target.enter[0]};
    XiCleanupRemap remap = {&value, 1u};
    char error[192];
    bool copied =
        xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
        xi_cleanup_verify(target.function, error, sizeof(error)) &&
        target.enter[0]->cleanup_boundary->kind == XI_CLEANUP_BOUNDARY_FATAL &&
        target.enter[0]->cleanup_boundary->leave == NULL;
    xi_func_free(source.function);
    xi_func_free(target.function);
    ASSERT(copied);
}

TEST(ordinary_phi_mapping_may_accompany_complete_boundary_map) {
    CleanupFixture source = cleanup_fixture_new(1u, false, true);
    CleanupFixture target = cleanup_fixture_new(1u, false, false);
    ASSERT(source.function && target.function);
    XiPhi *source_phi = xi_phi_new(source.function, source.function->entry, &cleanup_unit, 0u);
    XiPhi *target_phi = xi_phi_new(target.function, target.function->entry, &cleanup_unit, 0u);
    ASSERT(source_phi && target_phi);
    XiCleanupValueMapping values[CLEANUP_TEST_MAP_CAPACITY];
    uint32_t count = cleanup_fixture_map(&source, &target, values);
    values[count++] = (XiCleanupValueMapping) {&source_phi->value, &target_phi->value};
    XiCleanupRemap remap = {values, count};
    char error[192];
    bool copied =
        xi_cleanup_remap(target.function, source.function, &remap, error, sizeof(error)) &&
        xi_cleanup_verify(target.function, error, sizeof(error)) &&
        target_phi->value.cleanup_boundary == NULL;
    xi_func_free(source.function);
    xi_func_free(target.function);
    ASSERT(copied);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Xi cleanup identity graph");
RUN_TEST(complete_frontier_owns_copied_pairs);
RUN_TEST(fatal_and_closed_completions_are_distinct);
RUN_TEST(unowned_and_unaligned_records_are_rejected_before_read);
RUN_TEST(poisoned_member_identities_are_rejected_before_read);
RUN_TEST(pair_rank_and_frontier_corruption_is_rejected);
RUN_TEST(cross_function_and_cross_frontier_relations_are_rejected);
RUN_TEST(nonmarkers_cannot_carry_boundary_identity);
RUN_TEST(attachment_refuses_replacement_and_invalid_pairs);
RUN_TEST(complete_remap_survives_source_arena_destruction);
RUN_TEST(same_function_clone_has_separate_frontier_occurrence);
RUN_TEST(every_missing_marker_mapping_rejects_without_publication);
RUN_TEST(ambiguous_and_poisoned_mapping_rejects_without_publication);
RUN_TEST(complete_pairs_without_complete_frontier_cannot_be_cloned);
RUN_TEST(target_order_and_occupied_records_cannot_be_overwritten);
RUN_TEST(independent_frontier_subset_does_not_require_other_occurrences);
RUN_TEST(fatal_remap_needs_no_synthetic_leave);
RUN_TEST(ordinary_phi_mapping_may_accompany_complete_boundary_map);
TEST_MAIN_END()
