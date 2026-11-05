/* Current generic vectors and nominal arguments are checked on real owned input. */
#ifndef XIR_ERROR_SUBSTITUTION_CASES_H
#define XIR_ERROR_SUBSTITUTION_CASES_H
static void ed_substitution_controls(const XrXirCompileContext *work,XrXirEffects *effects) {
    EffectBodyMaterial *body=effects->invocations->body;CHECK(body->domain.types->count==2);
    CHECK(local_errors_owned(work,body,NULL)==XR_XIR_OK);
    XrXirType *argument=(XrXirType *)body->domain.types->nodes[1].nominal.arguments;
    CHECK(argument && argument[0]==XR_XIR_I64);argument[0]=XR_XIR_BOOL;
    CHECK(local_errors_owned(work,body,NULL)==XR_XIR_BAD_TYPE);argument[0]=XR_XIR_I64;
    XrXirType *actual=(XrXirType *)body->generics[1].arguments;
    CHECK(actual && actual[0]==XR_XIR_I64);actual[0]=XR_XIR_BOOL;
    CHECK(local_errors_owned(work,body,NULL)==XR_XIR_BAD_TYPE);actual[0]=XR_XIR_I64;
    CHECK(local_errors_owned(work,body,NULL)==XR_XIR_OK);
}
#endif
