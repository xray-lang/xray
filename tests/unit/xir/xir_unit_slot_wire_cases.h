/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_unit_slot_wire_cases.h - Independent schema field attacks
 *
 * KEY CONCEPT:
 *   Locate actual fields from bytes, then rehash attacks without the writer.
 */
#ifndef XIR_UNIT_SLOT_WIRE_CASES_H
#define XIR_UNIT_SLOT_WIRE_CASES_H
#include "base/xsha256.h"
#include "xir/xxir_types.h"
typedef struct UnitWireCursor {const uint8_t *bytes;size_t size,at;} UnitWireCursor;
typedef struct UnitWireOp {uint32_t function;size_t at;} UnitWireOp;
static uint64_t unit_wire_read(UnitWireCursor *c,unsigned width){
 CHECK(width<=8&&c->at<=c->size&&width<=c->size-c->at);uint64_t v=0;
 for(unsigned i=0;i<width;++i)v|=(uint64_t)c->bytes[c->at+i]<<(8*i);c->at+=width;return v;
}
static void unit_wire_skip(UnitWireCursor *c,uint64_t count,unsigned width){
 CHECK(width&&c->at<=c->size&&count<=(c->size-c->at)/width);c->at+=(size_t)count*width;
}
static void unit_wire_vector(UnitWireCursor *c,unsigned width){uint64_t count=unit_wire_read(c,4);unit_wire_skip(c,count,width);}
static void unit_wire_digest(uint8_t *bytes,size_t size){
 XrSHA256Context c;xr_sha256_init(&c);xr_sha256_update(&c,bytes,32);xr_sha256_update(&c,bytes+64,size-64);xr_sha256_final(&c,bytes+32);
}
static void unit_wire_attack(const uint8_t *original,size_t size,uint8_t *copy,size_t at,
 unsigned width,uint64_t value,XrXirStatus expected,bool faults){
 CHECK(at<=size&&width<=size-at);memcpy(copy,original,size);
 for(unsigned i=0;i<width;++i)copy[at+i]=(uint8_t)(value>>(8*i));unit_wire_digest(copy,size);
 size_t live=runtime_live,bytes=runtime_bytes,points=0;
 for(size_t pass=0;pass<=points;++pass){
  UnitCompileOwner owner;unit_compile_owner_new(&owner);
  effects_compile_attempts=0;effects_compile_injected=false;effects_compile_fail_at=pass?pass-1:SIZE_MAX;XrXirArtifact *artifact=NULL;
  XrXirStatus status=xr_xir_compile_checked_read(&owner.context,copy,size,&artifact,NULL);
  if(artifact||status!=(pass?XR_XIR_OUT_OF_MEMORY:expected)){XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);fprintf(stderr,"wire attack at=%zu width=%u value=%llu pass=%zu status=%u expected=%u work=%llu allocated=%llu\n",at,width,(unsigned long long)value,pass,status,pass?XR_XIR_OUT_OF_MEMORY:expected,(unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes);}CHECK(!artifact&&status==(pass?XR_XIR_OUT_OF_MEMORY:expected));
  if(pass)CHECK(effects_compile_injected);
  if(!pass&&faults)points=effects_compile_attempts;effects_compile_fail_at=SIZE_MAX;
  unit_compile_owner_free(&owner);CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
 if(faults)printf("Unit slot malformed packet %zu OOM points, no partial artifact and physical baseline restored\n",points);
}
typedef struct UnitWireFunction {
 size_t parameters_at,instructions_at;uint32_t parameters,instructions;
} UnitWireFunction;
typedef struct UnitWireType {uint32_t kind,element;size_t at;} UnitWireType;
typedef struct UnitWireLayout {
 const uint8_t *bytes;size_t size;UnitWireFunction *functions;UnitWireType *types;
 uint32_t function_count,type_count;
} UnitWireLayout;
static void unit_wire_application(UnitWireCursor *c){(void)unit_wire_read(c,4);unit_wire_vector(c,4);}
static void unit_wire_constraints(UnitWireCursor *c,uint32_t count){
 CHECK(count<=(c->size-c->at)/8);
 for(uint32_t p=0;p<count;++p){(void)unit_wire_read(c,4);uint32_t interfaces=(uint32_t)unit_wire_read(c,4);
  CHECK(interfaces<=(c->size-c->at)/8);for(uint32_t a=0;a<interfaces;++a)unit_wire_application(c);
 }
}
static void unit_wire_types(UnitWireCursor *c,uint32_t literals,UnitWireLayout *layout){
 for(uint32_t l=0;l<literals;++l)unit_wire_vector(c,1);
 uint32_t implementations=(uint32_t)unit_wire_read(c,4);CHECK(implementations<=(c->size-c->at)/16);
 for(uint32_t n=0;n<implementations;++n){(void)unit_wire_read(c,4);unit_wire_application(c);
  uint32_t bindings=(uint32_t)unit_wire_read(c,4);CHECK(bindings<=(c->size-c->at)/16);
  for(uint32_t b=0;b<bindings;++b){unit_wire_application(c);unit_wire_skip(c,2,4);}
 }
 uint32_t generic=(uint32_t)unit_wire_read(c,4);CHECK(generic<=1);
 if(generic)for(uint32_t f=0;f<layout->function_count;++f){
  uint32_t count=(uint32_t)unit_wire_read(c,4),kinds=(uint32_t)unit_wire_read(c,4);CHECK(kinds<=1);
  if(kinds)unit_wire_skip(c,count,4);unit_wire_constraints(c,count);unit_wire_vector(c,4);
 }
 layout->type_count=(uint32_t)unit_wire_read(c,4);unit_wire_skip(c,2,4);
 CHECK(layout->type_count<=(c->size-c->at)/12);
 layout->types=xr_malloc((layout->type_count?layout->type_count:1)*sizeof(*layout->types));CHECK(layout->types);
 for(uint32_t t=0;t<layout->type_count;++t){UnitWireType node={0,UINT32_MAX,c->at};
  node.kind=(uint32_t)unit_wire_read(c,4);(void)unit_wire_read(c,4);
  switch(node.kind){
  case XR_XIR_TYPE_CALLABLE:unit_wire_vector(c,8);unit_wire_skip(c,2,4);break;
  case XR_XIR_TYPE_TUPLE:unit_wire_vector(c,4);break;
  case XR_XIR_TYPE_ARRAY:case XR_XIR_TYPE_CELL:case XR_XIR_TYPE_NULLABLE:
  case XR_XIR_TYPE_ATOMIC:case XR_XIR_TYPE_TASK:node.element=(uint32_t)unit_wire_read(c,4);break;
  case XR_XIR_TYPE_NOMINAL:(void)unit_wire_read(c,4);unit_wire_vector(c,4);unit_wire_vector(c,4);break;
  default:CHECK(false);
  }
  layout->types[t]=node;
 }
}
static bool unit_wire_cell_unit(const UnitWireLayout *layout,uint32_t type){
 if(type<XR_XIR_CONSTRUCTED_TYPE_BASE||type-XR_XIR_CONSTRUCTED_TYPE_BASE>=layout->type_count)return false;
 const UnitWireType *node=&layout->types[type-XR_XIR_CONSTRUCTED_TYPE_BASE];
 return node->kind==XR_XIR_TYPE_CELL&&node->element==XR_XIR_UNIT;
}
static uint32_t unit_wire_value_type(const UnitWireLayout *layout,uint32_t f,uint32_t value){
 CHECK(f<layout->function_count);const UnitWireFunction *fn=&layout->functions[f];
 CHECK(value<fn->parameters+fn->instructions);
 size_t at=value<fn->parameters?fn->parameters_at+(size_t)value*4:
  fn->instructions_at+(size_t)(value-fn->parameters)*40+4;
 UnitWireCursor c={layout->bytes,layout->size,at};return (uint32_t)unit_wire_read(&c,4);
}
static unsigned unit_wire_cell_attacks(const UnitWireLayout *layout,const UnitWireOp *op,
 uint8_t *copy,unsigned counts[3]){
 UnitWireCursor c={layout->bytes,layout->size,op->at};uint32_t code=(uint32_t)unit_wire_read(&c,4);
 if(code!=XR_XIR_CELL_NEW&&code!=XR_XIR_CELL_READ&&code!=XR_XIR_CELL_WRITE)return 0;
 uint32_t type=(uint32_t)unit_wire_read(&c,4),input=(uint32_t)unit_wire_read(&c,4),payload=(uint32_t)unit_wire_read(&c,4);
 uint32_t physical=code==XR_XIR_CELL_NEW?type:unit_wire_value_type(layout,op->function,input);
 if(!unit_wire_cell_unit(layout,physical))return 0;
 unsigned kind=code==XR_XIR_CELL_NEW?0u:code==XR_XIR_CELL_READ?1u:2u;++counts[kind];
 CHECK(type==(kind?XR_XIR_UNIT:physical)&&!payload);unsigned attacks=0;
 if(!kind){CHECK(!input);unit_wire_attack(layout->bytes,layout->size,copy,op->at+8,4,1,XR_XIR_BAD_STRUCTURE,false);++attacks;}
 else{unit_wire_attack(layout->bytes,layout->size,copy,op->at+4,4,XR_XIR_I64,XR_XIR_BAD_TYPE,false);++attacks;}
 unit_wire_attack(layout->bytes,layout->size,copy,op->at+12,4,1,XR_XIR_BAD_STRUCTURE,false);++attacks;
 unit_wire_attack(layout->bytes,layout->size,copy,op->at,4,UINT32_MAX,XR_XIR_BAD_STRUCTURE,false);return attacks+1;
}
static void unit_slot_wire_cases(const uint8_t *bytes,size_t size){
 CHECK(size>=64&&size<=262144);UnitWireCursor c={bytes,size,64};CHECK(unit_wire_read(&c,4)==XR_XIR_PROGRAM);
 uint32_t functions=(uint32_t)unit_wire_read(&c,4);CHECK(unit_wire_read(&c,4)==1&&functions<=size/24);
 uint32_t *owners=malloc((functions?functions:1)*sizeof(*owners));CHECK(owners);
 UnitWireFunction *fns=xr_malloc((functions?functions:1)*sizeof(*fns));CHECK(fns);
 size_t capacity=size/40,used=0;UnitWireOp *ops=malloc((capacity?capacity:1)*sizeof(*ops));CHECK(ops);
 for(uint32_t f=0;f<functions;++f){
  unit_wire_vector(&c,1);fns[f].parameters=(uint32_t)unit_wire_read(&c,4);fns[f].parameters_at=c.at;
  unit_wire_skip(&c,fns[f].parameters,4);(void)unit_wire_read(&c,4);unit_wire_vector(&c,16);
  uint32_t instructions=(uint32_t)unit_wire_read(&c,4);CHECK(instructions<=capacity-used);
  fns[f].instructions=instructions;fns[f].instructions_at=c.at;
  for(uint32_t i=0;i<instructions;++i){size_t at=c.at;uint32_t op=(uint32_t)unit_wire_read(&c,4);unit_wire_skip(&c,36,1);
   if(op==XR_XIR_SLOT_LOAD||op==XR_XIR_SLOT_INIT||op==XR_XIR_SLOT_STORE||
    op==XR_XIR_CELL_NEW||op==XR_XIR_CELL_READ||op==XR_XIR_CELL_WRITE)ops[used++]=(UnitWireOp){f,at};
  }
  unit_wire_vector(&c,4);
 }
 uint32_t modules=(uint32_t)unit_wire_read(&c,4),slots=(uint32_t)unit_wire_read(&c,4),literals=(uint32_t)unit_wire_read(&c,4);
 unit_wire_skip(&c,2,4);CHECK(modules<=size/12&&slots<=size/12);
 for(uint32_t m=0;m<modules;++m){unit_wire_vector(&c,1);unit_wire_vector(&c,4);(void)unit_wire_read(&c,4);}
 for(uint32_t f=0;f<functions;++f){owners[f]=(uint32_t)unit_wire_read(&c,4);unit_wire_skip(&c,8,4);}
 size_t slot_at=c.at;unit_wire_skip(&c,slots,12);
 UnitWireLayout layout={bytes,size,fns,NULL,functions,0};unit_wire_types(&c,literals,&layout);
 uint8_t *copy=malloc(size);CHECK(copy);unsigned attacks=0,unit_writes=0,unit_reads=0,cell_writes=0,cell_reads=0,foreign=0,cell_ops[3]={0};
 for(size_t o=0;o<used;++o){
  unsigned cell_attacks=unit_wire_cell_attacks(&layout,&ops[o],copy,cell_ops);
  if(cell_attacks){attacks+=cell_attacks;continue;}
  UnitWireCursor in={bytes,size,ops[o].at};uint32_t op=(uint32_t)unit_wire_read(&in,4);
  if(op!=XR_XIR_SLOT_LOAD&&op!=XR_XIR_SLOT_INIT&&op!=XR_XIR_SLOT_STORE)continue;
  (void)unit_wire_read(&in,4);unit_wire_skip(&in,4,4);uint64_t slot=unit_wire_read(&in,8);CHECK(slot<slots);
  UnitWireCursor desc={bytes,size,slot_at+(size_t)slot*12};uint32_t owner=(uint32_t)unit_wire_read(&desc,4);
  uint32_t type=(uint32_t)unit_wire_read(&desc,4),mutable_slot=(uint32_t)unit_wire_read(&desc,4);
  bool cell=unit_wire_cell_unit(&layout,type);if(type!=XR_XIR_UNIT&&!cell)continue;
  CHECK(owner==owners[ops[o].function]&&mutable_slot==(uint32_t)cell&&op!=XR_XIR_SLOT_STORE);
  if(op==XR_XIR_SLOT_LOAD){if(cell)++cell_reads;else ++unit_reads;
   unit_wire_attack(bytes,size,copy,ops[o].at+4,4,XR_XIR_I64,XR_XIR_BAD_TYPE,false);++attacks;
  }else{
   if(cell){
    const UnitWireFunction *fn=&fns[ops[o].function];
    unit_wire_attack(bytes,size,copy,ops[o].at+8,4,(uint64_t)fn->parameters+fn->instructions,XR_XIR_BAD_VALUE,false);++attacks;
    unit_wire_attack(bytes,size,copy,ops[o].at+12,4,1,XR_XIR_BAD_STRUCTURE,!cell_writes);++cell_writes;++attacks;
   }else{
    unit_wire_attack(bytes,size,copy,ops[o].at+8,4,1,XR_XIR_BAD_STRUCTURE,!unit_writes);++unit_writes;++attacks;
    unit_wire_attack(bytes,size,copy,ops[o].at+12,4,1,XR_XIR_BAD_STRUCTURE,false);++attacks;
   }
   unit_wire_attack(bytes,size,copy,ops[o].at,4,XR_XIR_SLOT_STORE,XR_XIR_BAD_STRUCTURE,false);++attacks;
  }
  unit_wire_attack(bytes,size,copy,ops[o].at,4,UINT32_MAX,XR_XIR_BAD_STRUCTURE,false);++attacks;
  unit_wire_attack(bytes,size,copy,ops[o].at+24,8,slots,XR_XIR_BAD_STRUCTURE,false);++attacks;
  for(uint32_t other=0;other<slots;++other){UnitWireCursor d={bytes,size,slot_at+(size_t)other*12};
   uint32_t om=(uint32_t)unit_wire_read(&d,4);
   if(om!=owner){unit_wire_attack(bytes,size,copy,ops[o].at+24,8,other,XR_XIR_BAD_STRUCTURE,false);++attacks;++foreign;break;}
  }
 }
 CHECK(unit_writes&&cell_writes&&unit_writes+cell_writes>=2&&unit_reads&&cell_reads&&foreign>=2&&attacks>=10);
 CHECK(cell_ops[0]&&cell_ops[1]&&cell_ops[2]);
 for(uint32_t slot=0;slot<slots;++slot){UnitWireCursor d={bytes,size,slot_at+(size_t)slot*12};
  (void)unit_wire_read(&d,4);uint32_t type=(uint32_t)unit_wire_read(&d,4),mut=(uint32_t)unit_wire_read(&d,4);
  if(type!=XR_XIR_UNIT&&!unit_wire_cell_unit(&layout,type))continue;
  unit_wire_attack(bytes,size,copy,slot_at+(size_t)slot*12,4,modules,XR_XIR_BAD_STRUCTURE,false);
  unit_wire_attack(bytes,size,copy,slot_at+(size_t)slot*12+8,4,2,XR_XIR_BAD_STRUCTURE,false);
  unit_wire_attack(bytes,size,copy,slot_at+(size_t)slot*12+8,4,!mut,XR_XIR_BAD_TYPE,false);attacks+=3;
 }
 unit_wire_attack(bytes,size,copy,12,4,50,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,8,4,19,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,64,4,UINT32_MAX,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,12,4,49,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,12,4,48,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,8,4,18,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,20,4,1,XR_XIR_BAD_STRUCTURE,false);
 XrXirArtifact *artifact=NULL;size_t live=runtime_live,physical=runtime_bytes;UnitCompileOwner owner;unit_compile_owner_new(&owner);
 CHECK(xr_xir_compile_checked_read(&owner.context,bytes,size-1,&artifact,NULL)==XR_XIR_BAD_STRUCTURE&&!artifact);
 CHECK(runtime_live==live&&runtime_bytes==physical);unit_compile_owner_free(&owner);
 xr_free(layout.types);xr_free(fns);free(copy);free(ops);free(owners);
 printf("Unit const wire writes/reads=%u/%u; Cell<Unit> writes/reads=%u/%u new/read/write=%u/%u/%u foreign=%u\n",
  unit_writes,unit_reads,cell_writes,cell_reads,cell_ops[0],cell_ops[1],cell_ops[2],foreign);
 printf("Unit slot %u independently located field attacks and old revision/reserved/truncation PASS\n",attacks);
}
#endif
