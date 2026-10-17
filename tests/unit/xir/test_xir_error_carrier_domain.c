/* Current zero domains are independently checked before and after mutation. */
#define FINITE_PROOF_ENTRY finite_original_entry
#include "test_xir_error_finite_proof.c"
#undef FINITE_PROOF_ENTRY
static void carrier_domain(unsigned mode) {
    EdgeFixture fixture;pc_empty_fixture(&fixture,mode);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    XrXirEffects seed={.resources=context.resources,.count=fixture.module.function_count};
    CHECK(effect_errors_seed(&material->domain,&seed,&context)==XR_XIR_OK);
    EffectErrorFinite *proof=finite_prepare(&context,&fixture.module,seed.outcomes_work.atoms,seed.outcomes_work.atom_count);
    CHECK(error_finite_derive(&context,proof)==XR_XIR_OK);finite_verify(&context,proof,XR_XIR_OK);
    EffectErrorFiniteRead read={proof,&context,UINT32_MAX,NULL};uint64_t result=UINT64_MAX;
    CHECK(error_finite_word(&read,proof->rows[1].end-64,64,&result)==XR_XIR_OK && !result);
    uint32_t at=0;CHECK(error_finite_lower(&read,proof->rows[1].first,&at)==XR_XIR_OK);
    CHECK(at==proof->count || proof->facts[at].coordinate>=proof->rows[1].end);
    CHECK(error_finite_word(&read,proof->rows[2].end-64,64,&result)==XR_XIR_OK && result==8);
    uint64_t coordinates[]={proof->rows[1].first,
        error_finite_entry_coordinate(proof,1,0,0,0)+1,proof->rows[1].end-64+1};
    for(unsigned attack=0;attack<3;++attack) {
        bool changed=false;CHECK(finite_insert(&context,proof,coordinates[attack],1,&changed)==XR_XIR_OK && changed);
        finite_verify(&context,proof,XR_XIR_BAD_STRUCTURE);
        uint32_t found=0;CHECK(error_finite_lower(&read,coordinates[attack],&found)==XR_XIR_OK);
        CHECK(found<proof->count && proof->facts[found].coordinate==coordinates[attack]);
        memmove(proof->facts+found,proof->facts+found+1,(size_t)(proof->count-found-1)*sizeof(*proof->facts));
        --proof->count;finite_verify(&context,proof,XR_XIR_OK);
    }
    memset(&fixture,0xDA,sizeof(fixture));finite_verify(&context,proof,XR_XIR_OK);
    finite_clear(proof);effect_outcomes_clear(&seed.outcomes_work,seed.count);effect_body_material_free(material);
    pc_release(&context,baseline);
    printf("actual empty Error domain mode%u: literal0/nonempty8; reach/entry/summary forgery rejected; no callbacks; producer death; physical0\n",mode);
}
int main(int argc,char **argv) { CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<8);carrier_domain(mode);return 0; }
