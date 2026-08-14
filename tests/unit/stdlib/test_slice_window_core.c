/* Known-answer tests for the shared strict contiguous-window owner.
 *
 * Each helper below is one of the four hand-written rules the tree carried
 * before xi.slice.window had an owner: the VM opcode, the two inline forms AOT
 * CGen emitted, and the restricted C90 runtime helper. Replaying all four
 * against the owner over an exhaustive grid is what pins the claim that the
 * owner is the same rule, and shows exactly where the four disagreed. */

#include "../test_framework.h"
#include "shared/xr_slice_window_core.h"

#define OWNER_WINDOW(proof, length, start, count, data, element_size)                              \
    XR_SLICE_WINDOW_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_HI,                            \
                                XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_LO,                            \
                                XR_SEM_CONSUMER_SEMANTIC_PLAN, (proof), (length), (start),         \
                                (count), (data), (element_size))

/* The window each rule produced when it admitted the request. */
typedef struct WindowAnswer {
    bool admitted;
    int64_t byte_offset; /* relative to the source base, 0 when the base is kept */
    int64_t length;
} WindowAnswer;

/* The VM opcode before the owner existed. */
static WindowAnswer vm_rule(int64_t length, int64_t start, int64_t count, const void *data,
                            int64_t element_size) {
    WindowAnswer answer;
    answer.admitted = !(start < 0 || count < 0 || start > length || count > length - start);
    answer.byte_offset = 0;
    answer.length = 0;
    if (!answer.admitted)
        return answer;
    answer.length = count;
    if (data != NULL && count > 0)
        answer.byte_offset = start * element_size;
    return answer;
}

/* The inline form AOT CGen emitted for a run-time count. */
static WindowAnswer cgen_runtime_count_rule(int64_t length, int64_t start, int64_t count,
                                            const void *data, int64_t element_size) {
    WindowAnswer answer;
    answer.admitted =
        !(length < 0 || start < 0 || count < 0 || start > length || count > length - start);
    answer.byte_offset = 0;
    answer.length = 0;
    if (!answer.admitted)
        return answer;
    answer.length = count;
    if (data != NULL && count > 0)
        answer.byte_offset = start * element_size;
    return answer;
}

/* The inline form AOT CGen emitted when the plan had already resolved the
 * count to a non-negative constant. It advanced the base unconditionally and
 * told the optimizer the source pointer was not null - an assumption the check
 * above it never established. */
static WindowAnswer cgen_fixed_count_rule(int64_t length, int64_t start, int64_t count,
                                          const void *data, int64_t element_size) {
    WindowAnswer answer;
    (void) data;
    answer.admitted = !(start < 0 || start > length - count);
    answer.byte_offset = 0;
    answer.length = 0;
    if (!answer.admitted)
        return answer;
    answer.length = count;
    if (count > 0)
        answer.byte_offset = start * element_size;
    return answer;
}

/* The restricted C90 runtime helper. */
static WindowAnswer c90_rule(int64_t length, int64_t start, int64_t count, const void *data,
                             int64_t element_size) {
    WindowAnswer answer;
    answer.admitted = !(length < 0 || start < 0 || count < 0 || start > length ||
                        count > length - start || (count > 0 && data == NULL));
    answer.byte_offset = 0;
    answer.length = 0;
    if (!answer.admitted)
        return answer;
    answer.length = count;
    if (count > 0)
        answer.byte_offset = start * element_size;
    return answer;
}

static WindowAnswer owner_rule(int64_t length, int64_t start, int64_t count, const void *data,
                               int64_t element_size) {
    XrSliceWindowPlan plan =
        OWNER_WINDOW(XR_SLICE_WINDOW_PROOF_NONE, length, start, count, data, element_size);
    WindowAnswer answer;
    answer.admitted = plan.admitted;
    answer.byte_offset = plan.advances ? plan.byte_offset : 0;
    answer.length = plan.length;
    return answer;
}

static bool same_answer(WindowAnswer left, WindowAnswer right) {
    if (left.admitted != right.admitted)
        return false;
    if (!left.admitted)
        return true;
    return left.byte_offset == right.byte_offset && left.length == right.length;
}

#define GRID_LENGTH_MIN (-2)
#define GRID_LENGTH_MAX 6
#define GRID_BOUND_MIN (-2)
#define GRID_BOUND_MAX 7
#define ELEMENT_SIZE 8

static unsigned char storage[64];

TEST(a_source_with_storage_gets_the_same_window_from_every_rule) {
    const void *data = storage;
    int cells = 0;
    for (int64_t length = GRID_LENGTH_MIN; length <= GRID_LENGTH_MAX; length++) {
        for (int64_t start = GRID_BOUND_MIN; start <= GRID_BOUND_MAX; start++) {
            for (int64_t count = GRID_BOUND_MIN; count <= GRID_BOUND_MAX; count++) {
                WindowAnswer owner = owner_rule(length, start, count, data, ELEMENT_SIZE);
                ASSERT_TRUE(same_answer(owner, vm_rule(length, start, count, data, ELEMENT_SIZE)));
                ASSERT_TRUE(same_answer(
                    owner, cgen_runtime_count_rule(length, start, count, data, ELEMENT_SIZE)));
                ASSERT_TRUE(same_answer(owner, c90_rule(length, start, count, data, ELEMENT_SIZE)));
                cells++;
            }
        }
    }
    ASSERT_EQ_INT((GRID_LENGTH_MAX - GRID_LENGTH_MIN + 1) * (GRID_BOUND_MAX - GRID_BOUND_MIN + 1) *
                      (GRID_BOUND_MAX - GRID_BOUND_MIN + 1),
                  cells);
}

TEST(a_resolved_count_reaches_the_same_window_as_a_run_time_one) {
    const void *data = storage;
    for (int64_t length = GRID_LENGTH_MIN; length <= GRID_LENGTH_MAX; length++) {
        for (int64_t start = GRID_BOUND_MIN; start <= GRID_BOUND_MAX; start++) {
            /* CGen only took the resolved-count form for a non-negative count. */
            for (int64_t count = 0; count <= GRID_BOUND_MAX; count++) {
                WindowAnswer owner = owner_rule(length, start, count, data, ELEMENT_SIZE);
                ASSERT_TRUE(same_answer(
                    owner, cgen_fixed_count_rule(length, start, count, data, ELEMENT_SIZE)));
            }
        }
    }
}

TEST(a_positive_window_over_a_source_without_storage_is_rejected_everywhere) {
    /* This is the cell the four rules disagreed on. The restricted C90 runtime
     * rejected it; the VM and the run-time-count inline form admitted it and
     * handed back a view with no address; the resolved-count inline form
     * admitted it and told the optimizer the address was not null. The owner
     * takes the only rule that keeps the operation's promise - one successful
     * check proves every access inside [0, count). */
    for (int64_t length = 0; length <= GRID_LENGTH_MAX; length++) {
        for (int64_t start = 0; start <= length; start++) {
            for (int64_t count = 1; count <= length - start; count++) {
                WindowAnswer owner = owner_rule(length, start, count, NULL, ELEMENT_SIZE);
                ASSERT_FALSE(owner.admitted);
                ASSERT_TRUE(same_answer(owner, c90_rule(length, start, count, NULL, ELEMENT_SIZE)));
                ASSERT_TRUE(vm_rule(length, start, count, NULL, ELEMENT_SIZE).admitted);
                ASSERT_TRUE(
                    cgen_runtime_count_rule(length, start, count, NULL, ELEMENT_SIZE).admitted);
            }
        }
    }
}

TEST(an_empty_window_over_a_source_without_storage_stays_admissible) {
    for (int64_t length = 0; length <= GRID_LENGTH_MAX; length++) {
        for (int64_t start = 0; start <= length; start++) {
            WindowAnswer owner = owner_rule(length, start, 0, NULL, ELEMENT_SIZE);
            ASSERT_TRUE(owner.admitted);
            ASSERT_EQ_INT(0, (int) owner.length);
            /* The base is kept rather than formed past an address the source
             * does not have. */
            ASSERT_EQ_INT(0, (int) owner.byte_offset);
        }
    }
}

TEST(a_rejected_window_names_the_operand_that_did_not_fit) {
    const void *data = storage;
    XrSliceWindowPlan negative_start =
        OWNER_WINDOW(XR_SLICE_WINDOW_PROOF_NONE, 4, -1, 2, data, ELEMENT_SIZE);
    ASSERT_FALSE(negative_start.admitted);
    ASSERT_EQ_INT(-1, (int) negative_start.fault_operand);

    XrSliceWindowPlan start_past_end =
        OWNER_WINDOW(XR_SLICE_WINDOW_PROOF_NONE, 4, 9, 0, data, ELEMENT_SIZE);
    ASSERT_FALSE(start_past_end.admitted);
    ASSERT_EQ_INT(9, (int) start_past_end.fault_operand);

    XrSliceWindowPlan count_past_end =
        OWNER_WINDOW(XR_SLICE_WINDOW_PROOF_NONE, 4, 3, 3, data, ELEMENT_SIZE);
    ASSERT_FALSE(count_past_end.admitted);
    ASSERT_EQ_INT(3, (int) count_past_end.fault_operand);

    XrSliceWindowPlan negative_count =
        OWNER_WINDOW(XR_SLICE_WINDOW_PROOF_NONE, 4, 1, -2, data, ELEMENT_SIZE);
    ASSERT_FALSE(negative_count.admitted);
    ASSERT_EQ_INT(-2, (int) negative_count.fault_operand);
}

TEST(a_discharged_bounds_proof_selects_the_derivation_alone) {
    const void *data = storage;
    for (int64_t start = 0; start <= 4; start++) {
        for (int64_t count = 0; count <= 4; count++) {
            XrSliceWindowPlan proven = XR_SLICE_WINDOW_OWNER_APPLY_PROVEN(
                XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_HI, XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_LO,
                XR_SEM_CONSUMER_SEMANTIC_PLAN, XR_SLICE_WINDOW_PROOF_BOUNDS, 8, start, count, data,
                ELEMENT_SIZE);
            XrSliceWindowPlan probed =
                OWNER_WINDOW(XR_SLICE_WINDOW_PROOF_NONE, 8, start, count, data, ELEMENT_SIZE);
            ASSERT_TRUE(proven.admitted);
            ASSERT_TRUE(probed.admitted);
            ASSERT_EQ_INT((int) probed.length, (int) proven.length);
            ASSERT_EQ_INT((int) probed.advances, (int) proven.advances);
            ASSERT_EQ_INT((int) probed.byte_offset, (int) proven.byte_offset);
        }
    }
    /* A proof the plan discharged is a promise, so the owner does not re-probe
     * it: the derivation is produced for a request the probe would reject. */
    XrSliceWindowPlan out_of_range = XR_SLICE_WINDOW_OWNER_APPLY_PROVEN(
        XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_HI, XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_LO,
        XR_SEM_CONSUMER_SEMANTIC_PLAN, XR_SLICE_WINDOW_PROOF_BOUNDS, 2, 1, 4, data, ELEMENT_SIZE);
    ASSERT_TRUE(out_of_range.admitted);
    ASSERT_EQ_INT(4, (int) out_of_range.length);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Slice Window Core");
RUN_TEST(a_source_with_storage_gets_the_same_window_from_every_rule);
RUN_TEST(a_resolved_count_reaches_the_same_window_as_a_run_time_one);
RUN_TEST(a_positive_window_over_a_source_without_storage_is_rejected_everywhere);
RUN_TEST(an_empty_window_over_a_source_without_storage_stays_admissible);
RUN_TEST(a_rejected_window_names_the_operand_that_did_not_fit);
RUN_TEST(a_discharged_bounds_proof_selects_the_derivation_alone);
TEST_MAIN_END()
