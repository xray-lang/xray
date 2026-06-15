#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "xmalloc.h"
#include "xm_metadata_verify.h"

static int g_fail = 0;

static void check_bool(const char *label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail++;
    }
}

static void check_error(const char *label, XmMetaVerifyError actual, XmMetaVerifyError expected) {
    check_bool(label, actual == expected);
}

static XrStackMapTable *make_stack_map(uint32_t count) {
    size_t bytes = sizeof(XrStackMapTable) + (size_t) count * sizeof(XrStackMapEntry);
    XrStackMapTable *table = (XrStackMapTable *) xr_malloc(bytes);
    if (table == NULL)
        return NULL;
    table->magic = XR_STACK_MAP_MAGIC;
    table->count = count;
    table->frame_size = 64;
    for (uint32_t i = 0; i < count; i++) {
        table->entries[i].pc_offset = i * 4;
        table->entries[i].reg_bitmap = 0;
        table->entries[i].spill_bitmap = 0;
    }
    return table;
}

static void test_stackmap_verify(void) {
    uint32_t err_idx = 777;
    XrStackMapTable *table = make_stack_map(2);
    check_bool("stackmap allocation", table != NULL);
    if (table == NULL)
        return;

    check_error("stackmap valid", xm_verify_stackmap(table, 16, 4, &err_idx), XM_META_VERIFY_OK);
    check_error("stackmap null accepted", xm_verify_stackmap(NULL, 16, 4, &err_idx),
                XM_META_VERIFY_OK);

    table->magic = 0;
    check_error("stackmap bad magic", xm_verify_stackmap(table, 16, 4, &err_idx),
                XM_META_VERIFY_STACKMAP_BAD_MAGIC);
    table->magic = XR_STACK_MAP_MAGIC;

    table->count = XM_MAX_STACK_MAP_ENTRIES + 1;
    check_error("stackmap count overflow", xm_verify_stackmap(table, 16, 4, &err_idx),
                XM_META_VERIFY_STACKMAP_COUNT_OVERFLOW);
    table->count = 2;

    table->entries[1].pc_offset = 16;
    check_error("stackmap pc out of range", xm_verify_stackmap(table, 16, 4, &err_idx),
                XM_META_VERIFY_STACKMAP_PC_OUT_OF_RANGE);
    check_bool("stackmap pc out of range index", err_idx == 1);

    table->entries[1].pc_offset = 2;
    check_error("stackmap pc misaligned", xm_verify_stackmap(table, 16, 4, &err_idx),
                XM_META_VERIFY_STACKMAP_PC_MISALIGNED);

    table->entries[0].pc_offset = 4;
    table->entries[1].pc_offset = 4;
    check_error("stackmap pc not sorted", xm_verify_stackmap(table, 16, 4, &err_idx),
                XM_META_VERIFY_STACKMAP_PC_NOT_SORTED);

    xr_free(table);
}

static void test_deopt_verify(void) {
    uint32_t err_idx = 777;
    // slots is a pointer into the deopt-table block; supply real storage here.
    XmRtDeoptSlot slots0[1] = {0};
    XmRtDeoptEntry entries[2] = {0};
    entries[0].bc_pc = 8;
    entries[0].deopt_id = 0;
    entries[0].nslots = 1;
    entries[0].slots = slots0;
    entries[0].slots[0].loc_kind = (uint8_t) DEOPT_LOC_SPILL;
    entries[0].slots[0].loc.spill_offset = 16;
    entries[1].bc_pc = 12;
    entries[1].deopt_id = 1;

    check_error("deopt valid", xm_verify_deopt(entries, 2, &err_idx), XM_META_VERIFY_OK);

    entries[1].deopt_id = 0;
    check_error("deopt id mismatch", xm_verify_deopt(entries, 2, &err_idx),
                XM_META_VERIFY_DEOPT_ID_MISMATCH);
    entries[1].deopt_id = 1;

    entries[0].bc_pc = UINT32_MAX;
    check_error("deopt invalid bc pc", xm_verify_deopt(entries, 2, &err_idx),
                XM_META_VERIFY_DEOPT_BC_PC_INVALID);
    entries[0].bc_pc = 8;

    entries[0].slots[0].loc_kind = (uint8_t) DEOPT_LOC_CONST_PTR + 1;
    check_error("deopt loc kind", xm_verify_deopt(entries, 2, &err_idx),
                XM_META_VERIFY_DEOPT_SLOT_LOC_KIND);
    entries[0].slots[0].loc_kind = (uint8_t) DEOPT_LOC_SPILL;

    entries[0].slots[0].loc.spill_offset = 7;
    check_error("deopt spill offset", xm_verify_deopt(entries, 2, &err_idx),
                XM_META_VERIFY_DEOPT_SPILL_OFFSET);
}

static void test_osr_verify(void) {
    uint32_t err_idx = 777;
    XmOsrEntry entries[2] = {0};
    entries[0].bc_offset = 10;
    entries[0].entry_offset = 4;
    entries[1].bc_offset = 20;
    entries[1].entry_offset = 8;

    check_error("osr valid", xm_verify_osr(entries, 2, 16, 4, &err_idx), XM_META_VERIFY_OK);
    check_error("osr count overflow", xm_verify_osr(NULL, XM_MAX_OSR_ENTRIES + 1, 16, 4, &err_idx),
                XM_META_VERIFY_OSR_COUNT_OVERFLOW);

    entries[1].entry_offset = 16;
    check_error("osr entry oob", xm_verify_osr(entries, 2, 16, 4, &err_idx),
                XM_META_VERIFY_OSR_ENTRY_OOB);
    entries[1].entry_offset = 2;
    check_error("osr entry misaligned", xm_verify_osr(entries, 2, 16, 4, &err_idx),
                XM_META_VERIFY_OSR_ENTRY_MISALIGNED);
    entries[1].entry_offset = 8;

    entries[1].bc_offset = 10;
    check_error("osr duplicate bc", xm_verify_osr(entries, 2, 16, 4, &err_idx),
                XM_META_VERIFY_OSR_BC_DUP);
}

static void test_resume_verify(void) {
    uint32_t err_idx = 777;
    const char *err_kind = NULL;
    XmCodegenResult result = {0};
    result.code_size = 16;

    check_error("resume absent accepted", xm_verify_resume_entry(0, 16, 4, &err_idx),
                XM_META_VERIFY_OK);
    check_error("resume valid", xm_verify_resume_entry(4, 16, 4, &err_idx), XM_META_VERIFY_OK);
    check_error("resume x64 byte offset accepted", xm_verify_resume_entry(3, 16, 1, &err_idx),
                XM_META_VERIFY_OK);
    check_error("resume entry oob", xm_verify_resume_entry(16, 16, 4, &err_idx),
                XM_META_VERIFY_RESUME_ENTRY_OOB);
    check_bool("resume oob index", err_idx == 0);
    check_error("resume entry misaligned", xm_verify_resume_entry(2, 16, 4, &err_idx),
                XM_META_VERIFY_RESUME_ENTRY_MISALIGNED);

    result.nsuspend = 1;
    result.resume_entry_offset = 2;
    result.suspend_entries[0].cont_offset = 4;
    result.suspend_entries[0].smap_id = 0;
    result.suspend_entries[0].result_bc_slot = -1;
    result.suspend_entries[0].result_tag_offset = -1;
    check_error("metadata resume composite", xm_verify_metadata(&result, 4, &err_idx, &err_kind),
                XM_META_VERIFY_RESUME_ENTRY_MISALIGNED);
    check_bool("metadata resume kind", err_kind != NULL && err_kind[0] == 'r');
}

static void test_suspend_verify(void) {
    uint32_t err_idx = 777;
    const char *err_kind = NULL;
    XmCodegenResult result = {0};
    XrStackMapTable *table = make_stack_map(1);
    check_bool("suspend stackmap allocation", table != NULL);
    if (table == NULL)
        return;

    result.code_size = 32;
    result.stack_map = table;
    result.nsuspend = 1;
    result.resume_entry_offset = 16;
    result.suspend_entries[0].cont_offset = 8;
    result.suspend_entries[0].smap_id = 0;
    result.suspend_entries[0].result_bc_slot = -1;
    result.suspend_entries[0].result_tag_offset = -1;
    check_error("suspend valid", xm_verify_suspend(&result, 4, &err_idx), XM_META_VERIFY_OK);

    result.nsuspend = XM_MAX_SUSPEND_ENTRIES + 1;
    check_error("suspend count overflow", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_COUNT_OVERFLOW);
    result.nsuspend = 1;

    result.resume_entry_offset = 0;
    check_error("suspend missing resume", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_MISSING_RESUME);
    result.resume_entry_offset = 16;

    result.nsuspend = 0;
    check_error("suspend resume without points", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_RESUME_WITHOUT_POINTS);
    result.nsuspend = 1;

    result.suspend_entries[0].cont_offset = 32;
    check_error("suspend cont oob", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_CONT_OOB);
    result.suspend_entries[0].cont_offset = 2;
    check_error("suspend cont misaligned", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_CONT_MISALIGNED);
    result.suspend_entries[0].cont_offset = 8;

    result.suspend_entries[0].smap_id = 1;
    check_error("suspend smap oob", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_SMAP_OOB);
    result.suspend_entries[0].smap_id = 0;

    result.suspend_entries[0].result_tag_offset =
        (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) XR_JIT_MAX_VREG_TAGS;
    check_error("suspend tag offset", xm_verify_suspend(&result, 4, &err_idx),
                XM_META_VERIFY_SUSPEND_TAG_OFFSET);
    result.suspend_entries[0].result_tag_offset = -1;

    result.suspend_entries[0].cont_offset = 2;
    check_error("metadata suspend composite", xm_verify_metadata(&result, 4, &err_idx, &err_kind),
                XM_META_VERIFY_SUSPEND_CONT_MISALIGNED);
    check_bool("metadata suspend kind", err_kind != NULL && err_kind[0] == 's');

    xr_free(table);
}

static void test_wrapper_verify(void) {
    XmCodegenResult result = {0};
    result.code_size = 16;
    result.nosr = 1;
    result.osr_entries[0].bc_offset = 1;
    result.osr_entries[0].entry_offset = 32;
    result.success = true;

    check_bool("wrapper rejects invalid metadata", !xm_verify_metadata_or_fail(&result, 4, "test"));
    check_bool("wrapper marks failure", !result.success);
    check_bool("wrapper sets error", result.error != NULL);
}

int main(void) {
    test_stackmap_verify();
    test_deopt_verify();
    test_osr_verify();
    test_resume_verify();
    test_suspend_verify();
    test_wrapper_verify();

    if (g_fail > 0) {
        fprintf(stderr, "%d metadata verifier checks failed\n", g_fail);
        return 1;
    }
    fprintf(stderr, "metadata verifier checks passed\n");
    return 0;
}
