/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_record_identity_cases.h - Nominal identity, schema and receiving boundaries
 */
static void record_identity_cases(void) {
    XrLirModule *m = NULL, *copy = NULL;
    OK(xr_lir_module_create(NULL, &m)); OK(xr_lir_module_create(NULL, &copy));
    char module[] = "alpha", name[] = "Value", last[] = "z", first[] = "a";
    XrLirRecordField fields[] = {{last,1,XR_LIR_I64,XR_LIR_RECORD_MUTABLE}, {first,1,XR_LIR_STRING,XR_LIR_RECORD_PRIVATE}};
    XrLirRecordArgument actual = {XR_LIR_ARRAY,XR_LIR_I64};
    XrLirRecordIdentity identity = {.module=module,.module_length=5,.name=name,.name_length=5,
        .arguments=&actual,.argument_count=1,.exported=1,.flags=XR_LIR_RECORD_MODULE_LITERAL};
    XrLirType a, b, c, d, again, received;
    OK(xr_lir_record_type(m,&identity,fields,2,&a));
    OK(xr_lir_record_type(m,&identity,fields,2,&again)); CHECK(a==again);
    actual.array_element=XR_LIR_STRING;
    OK(xr_lir_record_type(m,&identity,fields,2,&b)); CHECK(a!=b);
    identity.module="other";
    OK(xr_lir_record_type(m,&identity,fields,2,&c)); CHECK(c!=a && c!=b);
    identity.name="Other";
    OK(xr_lir_record_type(m,&identity,fields,2,&d)); CHECK(d!=a && d!=b && d!=c);
    memset(module,'x',5); memset(name,'x',5); last[0]='x'; first[0]='x';
    actual=(XrLirRecordArgument){XR_LIR_PTR,0};
    OK(xr_lir_module_finish(m,NULL));
    OK(xr_lir_copy_types(copy,m));
    OK(xr_lir_import_type(copy,m,a,&received)); CHECK(a==received);
    xr_lir_module_destroy(m); m=NULL;
    OK(xr_lir_record_identity(copy,received,&identity));
    CHECK(identity.module_length==5 && !memcmp(identity.module,"alpha",5) &&
        identity.name_length==5 && !memcmp(identity.name,"Value",5) && identity.exported==1 &&
        identity.flags==XR_LIR_RECORD_MODULE_LITERAL && identity.argument_count==1 &&
        identity.arguments[0].type==XR_LIR_ARRAY && identity.arguments[0].array_element==XR_LIR_I64);
    const XrLirRecordField *owned; uint32_t count;
    OK(xr_lir_aggregate_fields(copy,received,&owned,&count));
    CHECK(count==2 && owned[0].name[0]=='z' && owned[1].name[0]=='a' &&
        owned[0].flags==XR_LIR_RECORD_MUTABLE && owned[1].flags==XR_LIR_RECORD_PRIVATE);
    OK(xr_lir_module_finish(copy,NULL)); OK(xr_lir_verify(copy,NULL)); xr_lir_module_destroy(copy);
    for (unsigned conflict=0;conflict<9;++conflict) {
        OK(xr_lir_module_create(NULL,&m));
        XrLirRecordField field={"value",5,XR_LIR_I64,0};
        identity=(XrLirRecordIdentity){.module="test",.module_length=4,.name="Value",.name_length=5};
        OK(xr_lir_record_type(m,&identity,&field,1,&a));
        if(conflict==0) field.type=XR_LIR_STRING;
        if(conflict==1) field.name="other";
        if(conflict==2) field.flags=XR_LIR_RECORD_MUTABLE;
        if(conflict==3) identity.exported=1;
        if(conflict==4) identity.flags=XR_LIR_RECORD_MODULE_LITERAL;
        if(conflict==5) { actual=(XrLirRecordArgument){XR_LIR_I64,0};identity.arguments=&actual;identity.argument_count=1; }
        if(conflict==6) identity.module="te\0t";
        if(conflict==7) identity.name=NULL;
        if(conflict==8) field.flags=XR_LIR_RECORD_PRIVATE|XR_LIR_RECORD_PROTECTED;
        XrLirType untouched=XR_LIR_I64;
        CHECK(xr_lir_record_type(m,&identity,&field,1,&untouched)==XR_LIR_INVALID_ARGUMENT && untouched==XR_LIR_I64);
        xr_lir_module_destroy(m); m=NULL;
    }
    OK(xr_lir_module_create(NULL,&m));
    XrLirRecordField duplicate[]={{"x",1,XR_LIR_I64,0},{"x",1,XR_LIR_I64,0}};
    identity=(XrLirRecordIdentity){.module="test",.module_length=4,.name="Bad",.name_length=3};
    CHECK(xr_lir_record_type(m,&identity,duplicate,2,&a)==XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
}
