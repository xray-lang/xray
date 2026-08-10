/* Standalone typed-slot architecture calibration. This file is deleted after
 * its governed measurement report is captured; it is never a product executor. */

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef enum Rep {
    REP_I32,
    REP_I64,
    REP_F32,
    REP_F64,
    REP_OBJECT,
    REP_DYNAMIC,
} Rep;

enum {
    SLOT_INITIALIZED = 1u << 0,
    SLOT_ROOT = 1u << 1,
    SLOT_OWNED = 1u << 2,
};

typedef struct Slot {
    uint16_t offset;
    uint8_t size;
    uint8_t align;
    uint8_t rep;
    uint8_t flags;
} Slot;

typedef enum OpKind {
    OP_CONST_I64,
    OP_ADD_I64,
    OP_INC_I64,
    OP_BRANCH_LT_I64,
    OP_OBJECT_NEW,
    OP_FIELD_STORE_I64,
    OP_FIELD_LOAD_I64,
    OP_ARRAY_SUM_I64,
    OP_MAP_GET_I64,
    OP_BOX_DYNAMIC_I64,
    OP_UNBOX_DYNAMIC_I64,
    OP_RETAIN,
    OP_RELEASE,
    OP_CALL_ADD_I64,
    OP_SUSPEND,
    OP_RETURN,
} OpKind;

typedef struct Op {
    uint8_t kind;
    uint8_t dst;
    uint8_t left;
    uint8_t right;
    int32_t immediate;
    uint32_t fingerprint;
} Op;

typedef struct Plan {
    const Slot *slots;
    uint8_t slot_count;
    const Op *ops;
    uint16_t op_count;
    uint16_t frame_size;
    uint32_t call_fingerprint;
    uint32_t decode_budget;
} Plan;

typedef struct Object {
    int32_t rc;
    int64_t field;
} Object;

typedef struct Dynamic {
    uint8_t tag;
    uint8_t reserved[7];
    int64_t payload;
} Dynamic;

typedef struct Events {
    uint64_t alloc;
    uint64_t retain;
    uint64_t release;
    uint64_t drop;
    uint64_t suspend;
    uint64_t resume;
} Events;

typedef struct Executor {
    uint8_t arena[64];
    uint16_t pc;
    bool suspended;
    Events events;
    Object object;
} Executor;

typedef enum VerifyError {
    VERIFY_OK,
    VERIFY_UNALIGNED_SLOT,
    VERIFY_WRONG_REP,
    VERIFY_MISSING_INIT,
    VERIFY_MISSING_ROOT,
    VERIFY_CALL_FINGERPRINT,
    VERIFY_DYNAMIC_ADAPTER,
    VERIFY_BUDGET,
} VerifyError;

static uint64_t monotonic_ns(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t) value.tv_sec * UINT64_C(1000000000) + (uint64_t) value.tv_nsec;
}

static int64_t load_i64(const Executor *executor, const Slot *slot) {
    int64_t value;
    memcpy(&value, executor->arena + slot->offset, sizeof(value));
    return value;
}

static void store_i64(Executor *executor, const Slot *slot, int64_t value) {
    memcpy(executor->arena + slot->offset, &value, sizeof(value));
}

static Object *load_object(const Executor *executor, const Slot *slot) {
    Object *value;
    memcpy(&value, executor->arena + slot->offset, sizeof(value));
    return value;
}

static void store_object(Executor *executor, const Slot *slot, Object *value) {
    memcpy(executor->arena + slot->offset, &value, sizeof(value));
}

static VerifyError verify_plan(const Plan *plan) {
    if (plan->op_count > plan->decode_budget)
        return VERIFY_BUDGET;
    for (uint8_t i = 0; i < plan->slot_count; i++) {
        const Slot *slot = &plan->slots[i];
        if (slot->align == 0 || slot->offset % slot->align != 0 ||
            (uint32_t) slot->offset + slot->size > plan->frame_size)
            return VERIFY_UNALIGNED_SLOT;
        if ((slot->flags & SLOT_INITIALIZED) == 0)
            return VERIFY_MISSING_INIT;
        if (slot->rep == REP_OBJECT && (slot->flags & SLOT_ROOT) == 0)
            return VERIFY_MISSING_ROOT;
    }
    for (uint16_t i = 0; i < plan->op_count; i++) {
        const Op *op = &plan->ops[i];
        if (op->kind == OP_CALL_ADD_I64 && op->fingerprint != plan->call_fingerprint)
            return VERIFY_CALL_FINGERPRINT;
        if (op->kind == OP_BOX_DYNAMIC_I64 || op->kind == OP_UNBOX_DYNAMIC_I64) {
            if (plan->slots[op->dst].rep !=
                (op->kind == OP_BOX_DYNAMIC_I64 ? REP_DYNAMIC : REP_I64))
                return VERIFY_DYNAMIC_ADAPTER;
        }
        if (op->kind == OP_ADD_I64 || op->kind == OP_INC_I64 || op->kind == OP_BRANCH_LT_I64 ||
            op->kind == OP_CALL_ADD_I64) {
            if (plan->slots[op->dst].rep != REP_I64 || plan->slots[op->left].rep != REP_I64)
                return VERIFY_WRONG_REP;
        }
    }
    return VERIFY_OK;
}

static int64_t array_sum_i64(const int64_t *values, uint32_t count) {
    int64_t result = 0;
    for (uint32_t i = 0; i < count; i++)
        result += values[i];
    return result;
}

static int64_t map_get_i64(const int64_t *keys, const int64_t *values, uint32_t count,
                           int64_t key) {
    for (uint32_t i = 0; i < count; i++) {
        if (keys[i] == key)
            return values[i];
    }
    return -1;
}

static bool execute(Executor *executor, const Plan *plan, int64_t *result) {
    static const int64_t array_values[] = {1, 2, 3, 4};
    static const int64_t map_keys[] = {3, 5, 7};
    static const int64_t map_values[] = {30, 50, 70};
    if (executor->suspended) {
        executor->suspended = false;
        executor->events.resume++;
    }
    while (executor->pc < plan->op_count) {
        const Op *op = &plan->ops[executor->pc++];
        const Slot *dst = &plan->slots[op->dst];
        const Slot *left = &plan->slots[op->left];
        const Slot *right = &plan->slots[op->right];
        switch (op->kind) {
            case OP_CONST_I64:
                store_i64(executor, dst, op->immediate);
                break;
            case OP_ADD_I64:
                store_i64(executor, dst, load_i64(executor, left) + load_i64(executor, right));
                break;
            case OP_INC_I64:
                store_i64(executor, dst, load_i64(executor, left) + 1);
                break;
            case OP_BRANCH_LT_I64:
                if (load_i64(executor, left) < load_i64(executor, right))
                    executor->pc = (uint16_t) op->immediate;
                break;
            case OP_OBJECT_NEW:
                executor->object.rc = 1;
                executor->object.field = 0;
                store_object(executor, dst, &executor->object);
                executor->events.alloc++;
                break;
            case OP_FIELD_STORE_I64:
                load_object(executor, dst)->field = load_i64(executor, left);
                break;
            case OP_FIELD_LOAD_I64:
                store_i64(executor, dst, load_object(executor, left)->field);
                break;
            case OP_ARRAY_SUM_I64:
                store_i64(executor, dst, array_sum_i64(array_values, 4));
                break;
            case OP_MAP_GET_I64:
                store_i64(executor, dst, map_get_i64(map_keys, map_values, 3, 5));
                break;
            case OP_BOX_DYNAMIC_I64: {
                Dynamic dynamic = {0};
                dynamic.tag = REP_I64;
                dynamic.payload = load_i64(executor, left);
                memcpy(executor->arena + dst->offset, &dynamic, sizeof(dynamic));
                break;
            }
            case OP_UNBOX_DYNAMIC_I64: {
                Dynamic dynamic;
                memcpy(&dynamic, executor->arena + left->offset, sizeof(dynamic));
                if (dynamic.tag != REP_I64)
                    return false;
                store_i64(executor, dst, dynamic.payload);
                break;
            }
            case OP_RETAIN:
                load_object(executor, left)->rc++;
                executor->events.retain++;
                break;
            case OP_RELEASE:
                if (--load_object(executor, left)->rc == 0)
                    executor->events.drop++;
                executor->events.release++;
                break;
            case OP_CALL_ADD_I64:
                store_i64(executor, dst, load_i64(executor, left) + op->immediate);
                break;
            case OP_SUSPEND:
                executor->suspended = true;
                executor->events.suspend++;
                return false;
            case OP_RETURN:
                *result = load_i64(executor, left);
                return true;
        }
    }
    return false;
}

typedef struct TaggedValue {
    uint64_t tag;
    int64_t payload;
} TaggedValue;

static int64_t tagged_scalar_loop(uint32_t iterations) {
    volatile TaggedValue counter = {REP_I64, 0};
    volatile TaggedValue sum = {REP_I64, 0};
    for (uint32_t i = 0; i < iterations; i++) {
        if (counter.tag != REP_I64 || sum.tag != REP_I64)
            return -1;
        sum.payload += counter.payload;
        counter.payload++;
    }
    return sum.payload;
}

static uint64_t benchmark_typed(const Plan *plan, uint32_t iterations, int64_t *checksum) {
    uint64_t start = monotonic_ns();
    int64_t total = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        Executor executor = {0};
        int64_t value = 0;
        execute(&executor, plan, &value);
        execute(&executor, plan, &value);
        total += value;
    }
    *checksum = total;
    return monotonic_ns() - start;
}

static uint64_t benchmark_tagged(uint32_t iterations, int64_t *checksum) {
    uint64_t start = monotonic_ns();
    *checksum = tagged_scalar_loop(iterations * 8u);
    return monotonic_ns() - start;
}

static uint64_t benchmark_mailbox(uint32_t messages, uint64_t *checksum) {
    volatile uint64_t queue[256] = {0};
    uint32_t head = 0;
    uint32_t tail = 0;
    uint64_t total = 0;
    uint64_t start = monotonic_ns();
    for (uint32_t i = 0; i < messages; i++) {
        queue[tail++ & 255u] = i;
        total += queue[head++ & 255u];
    }
    *checksum = total;
    return monotonic_ns() - start;
}

int main(void) {
    static const Slot slots[] = {
        {0, 8, 8, REP_I64, SLOT_INITIALIZED},
        {8, 8, 8, REP_I64, SLOT_INITIALIZED},
        {16, 8, 8, REP_I64, SLOT_INITIALIZED},
        {24, 8, 8, REP_OBJECT, SLOT_INITIALIZED | SLOT_ROOT | SLOT_OWNED},
        {32, 16, 8, REP_DYNAMIC, SLOT_INITIALIZED},
        {48, 8, 8, REP_I64, SLOT_INITIALIZED},
        {56, 8, 8, REP_I64, SLOT_INITIALIZED},
    };
    static const Op ops[] = {
        {OP_CONST_I64, 0, 0, 0, 0, 0},       {OP_CONST_I64, 1, 0, 0, 8, 0},
        {OP_CONST_I64, 2, 0, 0, 0, 0},       {OP_OBJECT_NEW, 3, 0, 0, 0, 0},
        {OP_ADD_I64, 2, 2, 0, 0, 0},         {OP_INC_I64, 0, 0, 0, 0, 0},
        {OP_BRANCH_LT_I64, 0, 0, 1, 4, 0},   {OP_ARRAY_SUM_I64, 5, 0, 0, 0, 0},
        {OP_MAP_GET_I64, 6, 0, 0, 0, 0},     {OP_ADD_I64, 2, 2, 5, 0, 0},
        {OP_ADD_I64, 2, 2, 6, 0, 0},         {OP_FIELD_STORE_I64, 3, 2, 0, 0, 0},
        {OP_FIELD_LOAD_I64, 5, 3, 0, 0, 0},  {OP_CALL_ADD_I64, 5, 5, 0, 1, UINT32_C(0x51a77e11)},
        {OP_BOX_DYNAMIC_I64, 4, 5, 0, 0, 0}, {OP_UNBOX_DYNAMIC_I64, 6, 4, 0, 0, 0},
        {OP_RETAIN, 3, 3, 0, 0, 0},          {OP_SUSPEND, 0, 0, 0, 0, 0},
        {OP_RELEASE, 3, 3, 0, 0, 0},         {OP_RELEASE, 3, 3, 0, 0, 0},
        {OP_RETURN, 0, 6, 0, 0, 0},
    };
    Plan plan = {slots, 7, ops, 21, 64, UINT32_C(0x51a77e11), 64};
    Executor executor = {0};
    int64_t result = 0;
    bool first = execute(&executor, &plan, &result);
    bool second = execute(&executor, &plan, &result);

    Slot bad_slots[7];
    Op bad_ops[21];
    memcpy(bad_slots, slots, sizeof(slots));
    memcpy(bad_ops, ops, sizeof(ops));
    Plan bad = plan;
    bad.slots = bad_slots;
    bad.ops = bad_ops;
    bad_slots[0].offset = 1;
    bool catches_unaligned = verify_plan(&bad) == VERIFY_UNALIGNED_SLOT;
    memcpy(bad_slots, slots, sizeof(slots));
    bad_slots[0].rep = REP_F64;
    bool catches_wrong_rep = verify_plan(&bad) == VERIFY_WRONG_REP;
    memcpy(bad_slots, slots, sizeof(slots));
    bad_slots[0].flags = 0;
    bool catches_missing_init = verify_plan(&bad) == VERIFY_MISSING_INIT;
    memcpy(bad_slots, slots, sizeof(slots));
    bad_slots[3].flags &= (uint8_t) ~SLOT_ROOT;
    bool catches_missing_root = verify_plan(&bad) == VERIFY_MISSING_ROOT;
    memcpy(bad_slots, slots, sizeof(slots));
    memcpy(bad_ops, ops, sizeof(ops));
    bad_ops[13].fingerprint++;
    bool catches_call = verify_plan(&bad) == VERIFY_CALL_FINGERPRINT;
    memcpy(bad_ops, ops, sizeof(ops));
    bad_ops[14].dst = 5;
    bool catches_dynamic = verify_plan(&bad) == VERIFY_DYNAMIC_ADAPTER;

    int64_t typed_checksum = 0;
    int64_t tagged_checksum = 0;
    uint64_t mailbox_checksum = 0;
    uint64_t typed_ns = benchmark_typed(&plan, 200000, &typed_checksum);
    uint64_t tagged_ns = benchmark_tagged(200000, &tagged_checksum);
    uint64_t mailbox_ns = benchmark_mailbox(1000000, &mailbox_checksum);
    bool mutations = catches_unaligned && catches_wrong_rep && catches_missing_init &&
                     catches_missing_root && catches_call && catches_dynamic;
    bool lifecycle = !first && second && result == 89 && executor.events.alloc == 1 &&
                     executor.events.retain == 1 && executor.events.release == 2 &&
                     executor.events.drop == 1 && executor.events.suspend == 1 &&
                     executor.events.resume == 1;

    printf("{\n");
    printf("  \"schema\": 1,\n");
    printf("  \"result\": \"%s\",\n",
           verify_plan(&plan) == VERIFY_OK && mutations && lifecycle ? "passed" : "failed");
    printf("  \"frame_bytes\": {\"typed\": %u, \"legacy_tagged\": %zu},\n", plan.frame_size,
           plan.slot_count * sizeof(TaggedValue));
    printf("  \"execution\": {\"result\": %" PRId64 ", \"alloc\": %" PRIu64 ", \"retain\": %" PRIu64
           ", \"release\": %" PRIu64 ", \"drop\": %" PRIu64 ", \"suspend\": %" PRIu64
           ", \"resume\": %" PRIu64 "},\n",
           result, executor.events.alloc, executor.events.retain, executor.events.release,
           executor.events.drop, executor.events.suspend, executor.events.resume);
    printf("  \"benchmarks\": {\"typed_plan_ns\": %" PRIu64 ", \"legacy_tagged_ns\": %" PRIu64
           ", \"mailbox_ns\": %" PRIu64 ", \"typed_checksum\": %" PRId64
           ", \"tagged_checksum\": %" PRId64 ", \"mailbox_checksum\": %" PRIu64 "},\n",
           typed_ns, tagged_ns, mailbox_ns, typed_checksum, tagged_checksum, mailbox_checksum);
    printf("  \"mutations\": {\"unaligned_slot\": %s, \"wrong_rep\": %s, "
           "\"missing_init\": %s, \"missing_root\": %s, \"wrong_call_fingerprint\": %s, "
           "\"bad_dynamic_adapter\": %s}\n",
           catches_unaligned ? "true" : "false", catches_wrong_rep ? "true" : "false",
           catches_missing_init ? "true" : "false", catches_missing_root ? "true" : "false",
           catches_call ? "true" : "false", catches_dynamic ? "true" : "false");
    printf("}\n");
    return verify_plan(&plan) == VERIFY_OK && mutations && lifecycle ? 0 : 1;
}
