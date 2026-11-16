/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Empty enum fixtures keep actual receiving identities and poisoned producers. */
#ifndef P09_ENUM_KEY_FIXTURE_H
#define P09_ENUM_KEY_FIXTURE_H
#include "xir/xxir_type_arena_internal.h"
typedef struct EnumKeyPool {XrXirTypeArena *arena;XrXirType key,map,array;} EnumKeyPool;
typedef struct EnumKeyShape {const char *module,*name;uint32_t shift;XrXirType argument;bool payload;} EnumKeyShape;
static XrXirValueStatus enum_key_pool_new(ValueCompileOwner *compiler,EnumKeyShape shape,EnumKeyPool *output) {
    char module[64],name[64],first[2]="A",second[2]="B",field_name[6]="value";
    if(!output||output->arena||shape.shift>1||strlen(shape.module)>=sizeof(module)||strlen(shape.name)>=sizeof(name))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    strcpy(module,shape.module);strcpy(name,shape.name);
    XrXirNominalVariant variants[]={{{first,1},0,0},{{second,1},0,shape.payload?1u:0u}};
    XrXirNominalFieldIdentity field={{field_name,5},0};
    XrXirNominalIdentity identity={.module={module,(uint32_t)strlen(module)},.name={name,(uint32_t)strlen(name)},
        .exported=1,.arity=shape.argument!=XR_XIR_UNIT,.fields=shape.payload?&field:NULL,.field_count=shape.payload?1u:0u,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=2};
    XrXirNominalTable table={NULL,1,&identity};XrXirType field_type=XR_XIR_I64,argument=shape.argument;
    XrXirType key=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+shape.shift);
    XrXirType map=(XrXirType)(key+1);XrXirTypeNode nodes[4]={{0},{0},{0},{0}};
    if(shape.shift)nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    nodes[shape.shift]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0,
        .arguments=shape.argument!=XR_XIR_UNIT?&argument:NULL,.argument_count=shape.argument!=XR_XIR_UNIT,
        .fields=shape.payload?&field_type:NULL,.field_count=shape.payload?1u:0u}};
    nodes[shape.shift+1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=key,.result=XR_XIR_I64};
    nodes[shape.shift+2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=key};
    XrXirTypes types={nodes,shape.shift+(shape.payload?1u:3u),&table,NULL};
    XrXirValueStatus status=xr_xir_compile_type_arena_new(&compiler->context,&types,&output->arena);
    if(status==XR_XIR_VALUE_OK){output->key=key;output->map=shape.payload?XR_XIR_UNIT:map;output->array=(XrXirType)(key+2);}
    memset(module,0xa5,sizeof(module));memset(name,0xa5,sizeof(name));memset(first,0xa5,sizeof(first));
    memset(second,0xa5,sizeof(second));memset(field_name,0xa5,sizeof(field_name));
    memset(nodes,0xa5,sizeof(nodes));memset(variants,0xa5,sizeof(variants));memset(&identity,0xa5,sizeof(identity));
    return status;
}
static void enum_key_pool_drop(EnumKeyPool *pool) {
    if(pool->arena)xr_xir_compile_type_arena_drop(pool->arena);
    *pool=(EnumKeyPool){0};
}
static XrXirValue enum_key_value(EnumKeyPool pool,uint32_t variant,XrXirDomain *domain) {
    XrXirValueAdmission admission={pool.arena,domain,NULL,NULL,UINT64_C(1000000),65536};XrXirValue value={0};
    CHECK(xr_xir_enum_new(pool.key,variant,NULL,0,&admission,&value)==XR_XIR_VALUE_OK);
    return value;
}
#endif
