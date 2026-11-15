/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Literal provenance remains owned across vector growth and rewrites. */
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
    XrLirModule *m=map_literal_program(0);XrRbcProgram *program=program_new(m,0);xr_lir_module_destroy(m);
    const XrRbcModule *view=xr_rbc_program_view(program);CHECK(view->function_count==1&&view->map_literal_count==2);
    XrRbcModule module=*view;XrRbcFunction function=view->functions[0];module.functions=&function;
    XrRbcArrayAccess *accesses=xr_malloc(function.array_access_count*sizeof(*accesses));
    uint32_t capacity=65537;size_t first=function.array_operand_count;
    XrRbcArrayLocation *operands=xr_malloc((first+2u*capacity)*sizeof(*operands));
    XrtMapLiteralOrigin *origins=xr_malloc((size_t)capacity*sizeof(*origins));CHECK(accesses&&operands&&origins);
    memcpy(accesses,function.array_accesses,function.array_access_count*sizeof(*accesses));
    memcpy(operands,function.array_operands,first*sizeof(*operands));
    size_t selected=SIZE_MAX;
    for(size_t n=0;n<function.array_access_count;++n)
        if(accesses[n].operation==XR_RBC_MAP_LITERAL&&accesses[n].kind==XR_RBC_MAP_STRING_I64){selected=n;break;}
    CHECK(selected!=SIZE_MAX&&accesses[selected].count==4);
    XrRbcArrayAccess *access=&accesses[selected];
    XrRbcArrayLocation key=function.array_operands[access->first],value=function.array_operands[access->first+1];
    XrtMapLiteral literals[]={view->map_literals[0],view->map_literals[1]};module.map_literals=literals;
    XrtMapLiteral *literal=&literals[access->cell-1];literal->items=origins;
    for(uint32_t n=0;n<capacity;++n) {
        origins[n]=(XrtMapLiteralOrigin){0,n+1,1};
        operands[first+2u*n]=key;operands[first+2u*n+1]=value;
    }
    function.array_accesses=accesses;function.array_operands=operands;access->first=(uint32_t)first;
    const uint32_t counts[]={32768,32769,65536,65537};
    XrtLocationOwner *locations=xr_rbc_program_locations(program);CHECK(locations);
    for(size_t n=0;n<sizeof(counts)/sizeof(counts[0]);++n) {
        literal->count=counts[n];access->count=2u*counts[n];function.array_operand_count=first+access->count;
        XrRbcExecution *execution=NULL;XrRbcStatus status=xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution);
        if(n<3) {
            if(status!=XR_RBC_OK)fprintf(stderr,"Map literal items %u operands %u admission %u\n",counts[n],access->count,(unsigned)status);
            CHECK(status==XR_RBC_OK&&execution);xr_rbc_execution_free(&execution);
        } else CHECK(status==XR_RBC_INVALID&&!execution);
    }
    xr_free(origins);xr_free(operands);xr_free(accesses);xr_rbc_program_destroy(program);
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
