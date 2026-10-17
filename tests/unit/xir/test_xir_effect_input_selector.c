/* An empty opcode result must still reject impossible owned storage. */
#define main selector_original_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
static void selector_no_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
int main(void) {
    EdgeFixture fixture;pc_empty_fixture(&fixture,4);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    EffectBodyRequest body=effect_body_owned_request(&context,material);
    EffectBodyFunction *head=&material->functions[1];EffectBodyFunction saved=*head;
    CHECK(head->instruction_count==1 && head->run_count && head->run_capacity && head->runs);
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    for(unsigned mode=0;mode<3;++mode) {
        if(mode==1) { head->instruction_count=0;head->run_count=0; }
        if(mode==2) *head=saved;
        EffectFinalInputSelect selected;XrXirInstruction op;uint32_t ordinal=UINT32_MAX;bool found=true;
        CHECK(effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&selected)==XR_XIR_OK);
        XrXirStatus actual=effect_final_input_select_next(&body,1,&selected,&ordinal,&op,&found);
        if(mode==1) {
            EffectBodyRecordCursor old;CHECK(effect_body_request_sequence_begin(&body,&old)==XR_XIR_OK);
            CHECK(effect_body_request_sequence_finish(&body,1,&old)==XR_XIR_BAD_STRUCTURE);
            fprintf(stderr,"actual zero instruction/nonzero owned storage selector status%u expected%u\n",actual,XR_XIR_BAD_STRUCTURE);
            CHECK(actual==XR_XIR_BAD_STRUCTURE);
        } else CHECK(actual==XR_XIR_OK && !found);
    }
    memset(&fixture,0xDC,sizeof(fixture));EffectFinalInputSelect selected;XrXirInstruction op;uint32_t ordinal=0;bool found=true;
    CHECK(effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&selected)==XR_XIR_OK);
    CHECK(effect_final_input_select_next(&body,1,&selected,&ordinal,&op,&found)==XR_XIR_OK && !found);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && physical==live_bytes);
    effect_body_material_free(material);pc_release(&context,baseline);
    puts("actual opcode selector: zero instruction/storage inconsistency rejected; restoration, producer death, no callbacks and physical0");return 0;
}
