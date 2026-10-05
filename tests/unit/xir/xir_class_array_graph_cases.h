/* Fixed fixture identities, including the governed Atomic Ordering dependency. */
#ifndef XIR_CLASS_ARRAY_GRAPH_CASES_H
#define XIR_CLASS_ARRAY_GRAPH_CASES_H
#include "xir/xxir_nominal.h"
enum {CLASS_ARRAY_ANSWER=4,CLASS_ARRAY_RETAINED=5,CLASS_ARRAY_SUSPENDED=6};
static void class_array_graph_verify(const XrXirModule *module) {
    const XrXirDeclarations *d=module->declarations;
    static const char *names[]={
        "module-id-v1:kind=6:script:namespace=0::path=7:root.xr",
        "module-id-v1:kind=6:script:namespace=0::path=8:value.xr",
        "module-id-v1:kind=6:script:namespace=0::path=9:reader.xr",
        "stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def"};
    C(d && d->root_module==0 && d->module_count==4 && module->function_count==19);
    for(uint32_t m=0;m<4;++m){
        C(d->modules[m].name_length==strlen(names[m]) &&
            !memcmp(d->modules[m].name,names[m],strlen(names[m])) && d->modules[m].initializer==m);
        C(module->functions[m].name_length==5 && !memcmp(module->functions[m].name,"$init",5));
        C(d->functions[m].module==m && !d->functions[m].exported);
    }
    C(d->modules[0].dependency_count==2 && d->modules[0].dependencies[0]==1 && d->modules[0].dependencies[1]==2);
    C(!d->modules[1].dependency_count && !d->modules[3].dependency_count);
    C(d->modules[2].dependency_count==2 && d->modules[2].dependencies[0]==1 && d->modules[2].dependencies[1]==3);
    const uint32_t functions[]={CLASS_ARRAY_ANSWER,CLASS_ARRAY_RETAINED,CLASS_ARRAY_SUSPENDED};
    static const char *roots[]={"answer","retained","suspended"};
    for(uint32_t i=0;i<3;++i){uint32_t f=functions[i];
        C(module->functions[f].name_length==strlen(roots[i]) && !memcmp(module->functions[f].name,roots[i],strlen(roots[i])));
        C(!module->functions[f].parameter_count && d->functions[f].module==d->root_module && d->functions[f].exported==1 &&
            !d->functions[f].nominal_owner && !d->functions[f].method_kind && !d->functions[f].test_role);
    }
    C(module->functions[CLASS_ARRAY_ANSWER].result==XR_XIR_I64 && module->functions[CLASS_ARRAY_SUSPENDED].result==XR_XIR_I64);
    const XrXirTypes *types=module->types;C(types && types->count==10 && types->nominals && types->nominals->count==4);
    C(types->nodes[0].kind==XR_XIR_TYPE_NOMINAL && types->nodes[0].nominal.declaration==0);
    C(types->nodes[1].kind==XR_XIR_TYPE_NOMINAL && types->nodes[1].nominal.declaration==1);
    C(types->nodes[0].nominal.field_count==1 && types->nodes[0].nominal.fields[0]==(XrXirType)261);
    C(types->nodes[1].nominal.field_count==1 && types->nodes[1].nominal.fields[0]==(XrXirType)260);
    C(types->nodes[2].kind==XR_XIR_TYPE_NOMINAL && types->nodes[2].nominal.declaration==2 && !types->nodes[2].nominal.field_count);
    C(types->nodes[4].kind==XR_XIR_TYPE_ARRAY && types->nodes[4].element==XR_XIR_STRING);
    C(types->nodes[5].kind==XR_XIR_TYPE_ARRAY && types->nodes[5].element==XR_XIR_I64);
    C(types->nodes[6].kind==XR_XIR_TYPE_ATOMIC && types->nodes[6].element==XR_XIR_I64);
    C(types->nodes[7].kind==XR_XIR_TYPE_CELL && types->nodes[7].element==(XrXirType)261);
    C(types->nodes[8].kind==XR_XIR_TYPE_CELL && types->nodes[8].element==(XrXirType)260);
    C(types->nodes[9].kind==XR_XIR_TYPE_NULLABLE && types->nodes[9].element==(XrXirType)259);
    C(module->functions[CLASS_ARRAY_RETAINED].result==(XrXirType)257);
    C(types->nodes[3].kind==XR_XIR_TYPE_NOMINAL && types->nodes[3].nominal.declaration==3 &&
        xr_xir_nominal_native_ordering(types,(XrXirType)259));
    for(uint32_t i=0;i<3;++i){
        const XrXirNominalNativeRecord *native=xr_xir_nominal_native_record(types,(XrXirType)(256+i));
        static const uint8_t zero[32]={0};C(native && !native->native_id && !memcmp(native->source_fingerprint,zero,32));
    }
}
#endif
