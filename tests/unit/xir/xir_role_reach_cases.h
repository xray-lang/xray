#include "xir_role_row_read_cases.h"
/* Physical Input determines unused, cyclic and propagated callable roles. */
static void pc_role_reach_fixture(InvocationRotationFixture *fixture,uint32_t mode) {
    invocation_rotation_fixture(fixture);
    if (mode) {
        fixture->rotate_operands[3]=3;
        fixture->roles[4].uses=0;
        fixture->contracts[1].formula.term_count=4;
        fixture->contracts[0].formula.constant_mask=0;
    }
}
static void pc_role_reach_literals(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        InvocationRotationFixture fixture;pc_role_reach_fixture(&fixture,mode);
        XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
        CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
        EffectInvocationCertificate *certificate=effects->invocations;
        CHECK(certificate && certificate->final->role_proof.rows && certificate->final->role_proof.facts);
        CHECK(effect_role_proof_match(&context,certificate->body,
            &certificate->final->role_proof)==XR_XIR_OK);
        pc_role_row_literals(&context,certificate);
        EffectRoleProof *proof=&certificate->final->role_proof;
        EffectRoleFact *claims=proof->facts+proof->rows[1].first;
        EffectRoleFact saved[5];memcpy(saved,claims,sizeof(saved));
        for (uint32_t p=0;p<5;++p) {
            CHECK(!saved[p].level && !saved[p].distance);
            for (uint32_t kind=0;kind<3;kind+=2) {
                claims[p]=(EffectRoleFact){kind==XR_XIR_EFFECT_PARAMETER_FIXED?2:1,1,UINT32_MAX,UINT32_MAX};
                CHECK(effect_role_proof_match(&context,certificate->body,
                    &certificate->final->role_proof)==XR_XIR_BAD_STRUCTURE);
                claims[p]=saved[p];
            }
        }
        for (uint32_t p=0;p<5;++p) claims[p]=(EffectRoleFact){2,1,1,(p+4)%5};
        CHECK(effect_role_proof_match(&context,certificate->body,
            &certificate->final->role_proof)==XR_XIR_BAD_STRUCTURE);
        memcpy(claims,saved,sizeof(saved));
        CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
        CHECK(effect_role_proof_match(&context,certificate->body,
            &certificate->final->role_proof)==XR_XIR_OK);
        fprintf(stderr,"physical role reachability mode%u five variable roles; ten false kinds and unsupported fixed cycle rejected; producer death passed\n",mode);
        xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    }
}
