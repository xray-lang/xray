/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Literal provenance remains owned across vector growth and rewrites. */
#include "vm/register/xrbc_code_owner.h"
static void map_literal_metadata(void) {
    XrLirModule *m=NULL,*copy=NULL;OK(xr_lir_module_create(NULL,&m));OK(xr_lir_module_create(NULL,&copy));
    char path[]="app.xr";XrLirMapLiteralOrigin items[]={{0,3,12},{1,4,9}};
    XrLirMapLiteral literal={path,6,items,2};uint32_t ordinal=77;
    OK(xr_lir_map_literal_add(m,&literal,&ordinal));CHECK(ordinal==1);
    memset(path,'x',sizeof(path));memset(items,0xa5,sizeof(items));
    for(unsigned n=0;n<128;++n) {
        /* Exercise an internal descriptor borrow across its vector's growth. */
        OK(xr_lir_map_literal_add(m,&m->map_literals[0],&ordinal));CHECK(ordinal==n+2);
    }
    XrLirFunction f=function(m,"metadata",NULL,0,XR_LIR_I64);XrLirBlock b=block(m,f,NULL,0);
    OK(xr_lir_set_entry(m,f,b));ret(m,b,constant(m,b,XR_LIR_I64,17));OK(xr_lir_module_finish(m,NULL));
    m=sccp_and_retire(m);CHECK(xr_lir_map_literal_count(m)==129);
    OK(xr_lir_map_literals_copy(copy,m));xr_lir_module_destroy(m);m=NULL;
    OK(xr_lir_module_finish(copy,NULL));
    for(uint32_t n=1;n<=129;++n) {
        OK(xr_lir_map_literal_view(copy,n,&literal));
        CHECK(literal.path_length==6 && !memcmp(literal.path,"app.xr",6) && literal.count==2 &&
            literal.items[0].kind==0 && literal.items[0].line==3 && literal.items[0].column==12 &&
            literal.items[1].kind==1 && literal.items[1].line==4 && literal.items[1].column==9);
    }
    OK(xr_lir_verify(copy,NULL));
    XrLirMapLiteralOrigin *owned=(XrLirMapLiteralOrigin *)copy->map_literals[0].items;
    owned[0].kind=2;CHECK(xr_lir_verify(copy,NULL)==XR_LIR_VERIFY_ERROR);owned[0].kind=0;
    xr_lir_module_destroy(copy);
    for(unsigned bad=0;bad<6;++bad) {
        OK(xr_lir_module_create(NULL,&m));XrLirMapLiteralOrigin origin={0,1,1};
        literal=(XrLirMapLiteral){"app.xr",6,&origin,1};
        if(bad==0)literal.path=NULL;
        if(bad==1)literal.items=NULL;
        if(bad==2)literal.path="a\0p.xr";
        if(bad==3)origin.kind=2;
        if(bad==4)origin.line=0;
        if(bad==5)origin.column=0;
        ordinal=77;CHECK(xr_lir_map_literal_add(m,&literal,&ordinal)==XR_LIR_INVALID_ARGUMENT && ordinal==77);
        xr_lir_module_destroy(m);m=NULL;
    }
}
static XrLirValue map_literal_value(XrLirModule *m,XrLirBlock b,XrLirOp op,XrLirType type,const XrLirValue *args,uint32_t count,uint64_t bits) {
    XrLirValue value={0};XrLirInstruction instruction={.op=op,.type=type,.operands=args,.operand_count=count,.bits=bits};
    OK(xr_lir_emit_value(m,b,&instruction,&value));return value;
}
static void map_literal_drop(XrLirModule *m,XrLirBlock b,XrLirOp op,XrLirValue value) {
    XrLirInstruction instruction={.op=op,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_effect(m,b,&instruction));
}
static XrLirValue map_literal_call(XrLirModule *m,XrLirBlock b,XrLirMapOperation role,const XrLirValue *args,uint32_t count) {
    uint32_t id=0;OK(xr_lir_map_operation(m,role,XR_LIR_MAP_STRING_I64,&id));
    XrLirValue value={0};XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,.intrinsic=id,.operands=args,.operand_count=count};
    OK(xr_lir_emit_value(m,b,&instruction,&value));return value;
}
static XrLirModule *map_literal_program(unsigned mode) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    OK(xr_lir_map_catalog_install(m,false,false,false,false,false));
    XrLirFunction f=function(m,"map_literal",NULL,0,XR_LIR_I64);XrLirBlock b=block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,b));
    XrLirMapLiteralOrigin origins[]={{0,3,12},{0,3,20},{0,5,16}};
    char path[]="map-\xc3\xa9.xr";XrLirMapLiteral literal={path,(uint32_t)sizeof(path)-1,mode==2?NULL:origins,mode==2?0:2};uint32_t first=0,last=0;
    bool duplicate=mode==1 || mode==3;if(mode==3){literal.path=NULL;literal.path_length=0;}
    OK(xr_lir_map_literal_add(m,&literal,&first));
    XrLirValue result={0};
    if(mode==2) {
        XrLirValue map=map_literal_value(m,b,XR_LIR_MAP_LITERAL_NEW,XR_LIR_MAP_STRING_I64,NULL,0,first);
        result=map_literal_call(m,b,XR_LIR_MAP_OP_LEN,&map,1);map_literal_drop(m,b,XR_LIR_MAP_DROP,map);
    } else {
        XrLirLiteral x={0},y={0};OK(xr_lir_add_literal(m,"x",1,&x));OK(xr_lir_add_literal(m,"y",1,&y));
        XrLirValue keyx=map_literal_value(m,b,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,x.id);
        XrLirValue keyy=map_literal_value(m,b,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,y.id);
        XrLirValue one=constant(m,b,XR_LIR_I64,1),two=constant(m,b,XR_LIR_I64,2),four=constant(m,b,XR_LIR_I64,4);
        XrLirValue initial[]={keyx,one,keyy,two};
        XrLirValue base=map_literal_value(m,b,XR_LIR_MAP_LITERAL_NEW,XR_LIR_MAP_STRING_I64,initial,4,first);
        literal.count=duplicate?3:2;origins[duplicate?1:0].kind=1;
        OK(xr_lir_map_literal_add(m,&literal,&last));
        XrLirValue args[]={keyx,one,base,keyx,four};
        XrLirValue map=map_literal_value(m,b,XR_LIR_MAP_LITERAL_NEW,XR_LIR_MAP_STRING_I64,
            duplicate?args:args+2,duplicate?5:3,last);
        XrLirValue lookup[]={map,keyx};XrLirValue left=map_literal_call(m,b,XR_LIR_MAP_OP_INDEX,lookup,2);
        lookup[1]=keyy;XrLirValue right=map_literal_call(m,b,XR_LIR_MAP_OP_INDEX,lookup,2);
        XrLirValue multiply[]={left,constant(m,b,XR_LIR_I64,10)};
        left=map_literal_value(m,b,XR_LIR_MUL,XR_LIR_I64,multiply,2,0);
        XrLirValue sum[]={left,right};result=map_literal_value(m,b,XR_LIR_ADD,XR_LIR_I64,sum,2,0);
        map_literal_drop(m,b,XR_LIR_MAP_DROP,map);map_literal_drop(m,b,XR_LIR_MAP_DROP,base);
        map_literal_drop(m,b,XR_LIR_STRING_DROP,keyx);map_literal_drop(m,b,XR_LIR_STRING_DROP,keyy);
    }
    memset(path,0xa5,sizeof(path));memset(origins,0xa5,sizeof(origins));
    ret(m,b,result);OK(xr_lir_module_finish(m,NULL));return m;
}
static void map_literal_operand_boundaries(void) {
    const uint32_t counts[]={32768,32769,65536,65537};
    for(size_t test=0;test<sizeof(counts)/sizeof(counts[0]);++test) {
        uint32_t count=counts[test],operand_count=2u*count;
        XrLirMapLiteralOrigin *origins=xr_malloc((size_t)count*sizeof(*origins));
        XrLirValue *operands=xr_malloc((size_t)operand_count*sizeof(*operands));CHECK(origins&&operands);
        XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
        XrLirFunction f=function(m,"wide_map_literal",NULL,0,XR_LIR_I64);
        XrLirBlock b=block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,b));
        XrLirLiteral text={0};OK(xr_lir_add_literal(m,"x",1,&text));
        XrLirValue key=map_literal_value(m,b,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,text.id);
        XrLirValue value=constant(m,b,XR_LIR_I64,42);
        for(uint32_t item=0;item<count;++item) {
            origins[item]=(XrLirMapLiteralOrigin){0,item+1,1};
            operands[2u*item]=key;operands[2u*item+1]=value;
        }
        XrLirMapLiteral literal={"wide.xr",7,origins,count};uint32_t ordinal=0;
        OK(xr_lir_map_literal_add(m,&literal,&ordinal));
        XrLirValue map=map_literal_value(m,b,XR_LIR_MAP_LITERAL_NEW,XR_LIR_MAP_STRING_I64,
            operands,operand_count,ordinal);
        memset(origins,0xa5,(size_t)count*sizeof(*origins));xr_free(origins);
        memset(operands,0xa5,(size_t)operand_count*sizeof(*operands));xr_free(operands);
        map_literal_drop(m,b,XR_LIR_MAP_DROP,map);map_literal_drop(m,b,XR_LIR_STRING_DROP,key);
        ret(m,b,value);OK(xr_lir_module_finish(m,NULL));OK(xr_lir_verify(m,NULL));
        XrRbcProgram *program=program_new(m,0);xr_lir_module_destroy(m);
        const XrRbcModule *view=xr_rbc_program_view(program);
        CHECK(view&&view->function_count==1&&view->map_literal_count==1&&
            view->code_lease.owner&&view->code_lease.retain&&view->code_lease.release);
        CHECK(ordinal==1&&view->map_literals[0].count==count&&view->map_literals[0].items&&
            view->map_literals[0].items[count-1].line==count);
        const XrRbcValueCode *code=xr_rbc_module_value_code(view,0);
        const XrtValueImage *image=xr_rbc_module_value_image(view);
        CHECK(code&&code->homes&&code->homes->values&&code->arguments&&code->accesses&&code->locations&&
            image&&image->types&&map.id<=code->value_count&&key.id<=code->value_count&&value.id<=code->value_count&&
            code->homes->value_count==code->value_count&&code->homes->frame.owned_offsets);
        size_t selected=SIZE_MAX;
        for(size_t index=0;index<code->access_count;++index)
            if(code->accesses[index].operation==XR_RBC_VALUE_MAP_LITERAL){CHECK(selected==SIZE_MAX);selected=index;}
        CHECK(selected!=SIZE_MAX);
        XrRbcValueAccess access=code->accesses[selected];
        CHECK(access.index==ordinal&&access.result==map.id&&access.count==operand_count&&
            access.first<=code->argument_count&&access.count<=code->argument_count-access.first);
        for(uint32_t item=0;item<count;++item)
            CHECK(code->arguments[access.first+2u*item]==key.id&&code->arguments[access.first+2u*item+1]==value.id);
        const XrLirValueHome *home=&code->homes->values[map.id],*key_home=&code->homes->values[key.id];
        CHECK(home->type&&home->type<=image->type_count&&image->types[home->type-1].kind==XRT_VALUE_MAP_STRING_I64&&
            home->owner_count==1&&home->first_owner<code->homes->frame.slot_count&&
            home->temporary_first_owner<code->homes->frame.slot_count&&
            home->first_owner!=home->temporary_first_owner&&home->offset!=home->temporary_offset&&
            code->homes->frame.owned_offsets[home->first_owner]==home->offset&&
            code->homes->frame.owned_offsets[home->temporary_first_owner]==home->temporary_offset&&
            code->locations[map.id].kind==XR_RBC_VALUE_NOWHERE);
        CHECK(key_home->type&&key_home->type<=image->type_count&&
            image->types[key_home->type-1].kind==XRT_VALUE_STRING&&!code->homes->values[value.id].type);
        size_t encoded=0;
        for(size_t pc=0;pc<view->functions[0].code_count;++pc) {
            uint32_t word=view->functions[0].code[pc];
            if((word&255)==XR_RBC_VALUE_ACCESS&&(word>>16)==selected)++encoded;
        }
        CHECK(encoded==1);
        XrtLocationOwner *locations=xr_rbc_program_locations(program);CHECK(locations);
        XrRbcExecution *execution=NULL;
        XrRbcStatus status=xr_rbc_module_prepare(view,0,NULL,0,locations,&execution);
        if(status!=XR_RBC_OK)fprintf(stderr,"Map literal items %u operands %u admission %u\n",count,operand_count,(unsigned)status);
        CHECK(status==XR_RBC_OK&&execution);xr_rbc_execution_free(&execution);
        /* Borrowing the exact descriptions cannot replace the real code owner. */
        XrRbcModule raw=*view;raw.code_lease=(XrRbcCodeLease){0};
        raw.borrowed_value_codes=&code;raw.borrowed_value_code_count=1;raw.borrowed_value_image=image;
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,locations,&execution)==XR_RBC_INVALID&&!execution);
        xr_rbc_program_destroy(program);
    }
}
static void map_literal_products(unsigned mode,bool rewrite,const char *path) {
    XrLirModule *m=map_literal_program(mode);if(rewrite)m=sccp_and_retire(m);
    XrRbcProgram *program=program_new(m,0);
    if(path)emit_c(m,path);else xr_lir_module_destroy(m);
    if(mode==0) {
        const XrRbcModule *view=xr_rbc_program_view(program);CHECK(view->map_literal_count==2);
        XrtLocationOwner *locations=xr_rbc_program_locations(program);CHECK(locations);
        XrRbcExecution *valid=NULL;
        CHECK(xr_rbc_module_prepare(view,0,NULL,0,locations,&valid)==XR_RBC_OK&&valid);
        xr_rbc_execution_free(&valid);
        for(unsigned bad=0;bad<8;++bad) {
            XrRbcModule module=*view;XrtMapLiteral literals[]={view->map_literals[0],view->map_literals[1]};
            XrtMapLiteralOrigin origins[]={{0,3,12},{0,3,20}};literals[0].items=origins;module.map_literals=literals;
            if(bad==0)module.map_literals=NULL;
            if(bad==1)module.map_literal_count=0;
            if(bad==2){literals[0].path="\xff";literals[0].path_length=1;}
            if(bad==3){literals[0].path="a\0b";literals[0].path_length=3;}
            if(bad==4)origins[0].kind=2;
            if(bad==5)origins[0].column=0;
            if(bad==6)literals[0].count=65537;
            if(bad==7)origins[0].kind=1;
            XrRbcExecution *execution=NULL;
            CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_INVALID && !execution);
        }
    }
    if(mode!=1 && mode!=3)check_vm(program,NULL,0,mode==2?0:42);
    else {
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);xr_rbc_program_destroy(program);program=NULL;
        uint32_t code=0,source=0;const XrtLocationOwner *locations=NULL;const char *message=NULL;size_t length=0;
        CHECK(xr_rbc_outcome_message_panic(outcome,&code,&message,&length,&source,&locations) && code==434 && !source && locations);
        const char *expected=mode==3?"duplicate key \"x\" in Map literal (entries at 3:12 and 5:16)":
            "duplicate key \"x\" in Map literal (entries at map-\xc3\xa9.xr:3:12 and map-\xc3\xa9.xr:5:16)";
        CHECK(length==strlen(expected) && !memcmp(message,expected,length));xr_rbc_outcome_free(&outcome);
    }
    xr_rbc_program_destroy(program);
}
