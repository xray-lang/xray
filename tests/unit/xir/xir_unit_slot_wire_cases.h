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
  runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;XrXirArtifact *artifact=NULL;
  XrXirStatus status=xr_xir_checked_read(copy,size,NULL,&artifact,NULL);
  CHECK(!artifact&&status==(pass?XR_XIR_OUT_OF_MEMORY:expected));
  CHECK(runtime_live==live&&runtime_bytes==bytes);
  if(!pass&&faults)points=runtime_attempts;
 }
 runtime_fail_at=SIZE_MAX;
 if(faults)printf("Unit slot malformed packet %zu OOM points, no partial artifact and physical baseline restored\n",points);
}
static void unit_slot_wire_cases(const uint8_t *bytes,size_t size){
 CHECK(size>=64&&size<=262144);UnitWireCursor c={bytes,size,64};
 uint32_t functions=(uint32_t)unit_wire_read(&c,4);CHECK(unit_wire_read(&c,4)==1&&functions<=size/24);
 uint32_t *owners=malloc((functions?functions:1)*sizeof(*owners));CHECK(owners);
 size_t capacity=size/40,used=0;UnitWireOp *ops=malloc((capacity?capacity:1)*sizeof(*ops));CHECK(ops);
 for(uint32_t f=0;f<functions;++f){
  unit_wire_vector(&c,1);unit_wire_vector(&c,4);(void)unit_wire_read(&c,4);unit_wire_vector(&c,16);
  uint32_t instructions=(uint32_t)unit_wire_read(&c,4);CHECK(instructions<=capacity-used);
  for(uint32_t i=0;i<instructions;++i){size_t at=c.at;uint32_t op=(uint32_t)unit_wire_read(&c,4);unit_wire_skip(&c,36,1);
   if(op==XR_XIR_SLOT_LOAD||op==XR_XIR_SLOT_INIT||op==XR_XIR_SLOT_STORE)ops[used++]=(UnitWireOp){f,at};
  }
  unit_wire_vector(&c,4);
 }
 uint32_t modules=(uint32_t)unit_wire_read(&c,4),slots=(uint32_t)unit_wire_read(&c,4);
 (void)unit_wire_read(&c,4);(void)unit_wire_read(&c,4);(void)unit_wire_read(&c,4);
 CHECK(modules<=size/12&&slots<=size/12);
 for(uint32_t m=0;m<modules;++m){unit_wire_vector(&c,1);unit_wire_vector(&c,4);(void)unit_wire_read(&c,4);}
 for(uint32_t f=0;f<functions;++f){owners[f]=(uint32_t)unit_wire_read(&c,4);unit_wire_skip(&c,6,4);}
 size_t slot_at=c.at;unit_wire_skip(&c,slots,12);
 uint8_t *copy=malloc(size);CHECK(copy);unsigned attacks=0,unit_writes=0,unit_reads=0;
 for(size_t o=0;o<used;++o){UnitWireCursor in={bytes,size,ops[o].at};uint32_t op=(uint32_t)unit_wire_read(&in,4);
  (void)unit_wire_read(&in,4);unit_wire_skip(&in,4,4);uint64_t slot=unit_wire_read(&in,8);CHECK(slot<slots);
  UnitWireCursor desc={bytes,size,slot_at+(size_t)slot*12};uint32_t owner=(uint32_t)unit_wire_read(&desc,4);
  uint32_t type=(uint32_t)unit_wire_read(&desc,4),mutable_slot=(uint32_t)unit_wire_read(&desc,4);
  if(type!=XR_XIR_UNIT)continue;CHECK(owner==owners[ops[o].function]);
  if(op==XR_XIR_SLOT_LOAD){++unit_reads;unit_wire_attack(bytes,size,copy,ops[o].at+4,4,XR_XIR_I64,XR_XIR_BAD_TYPE,false);++attacks;}
  else{unit_wire_attack(bytes,size,copy,ops[o].at+8,4,1,XR_XIR_BAD_STRUCTURE,!unit_writes);++unit_writes;++attacks;
   unit_wire_attack(bytes,size,copy,ops[o].at+12,4,1,XR_XIR_BAD_STRUCTURE,false);++attacks;
   if(!mutable_slot){unit_wire_attack(bytes,size,copy,ops[o].at,4,XR_XIR_SLOT_STORE,XR_XIR_BAD_STRUCTURE,false);++attacks;}
  }
  unit_wire_attack(bytes,size,copy,ops[o].at,4,UINT32_MAX,XR_XIR_BAD_STRUCTURE,false);++attacks;
  for(uint32_t other=0;other<slots;++other){UnitWireCursor d={bytes,size,slot_at+(size_t)other*12};
   uint32_t om=(uint32_t)unit_wire_read(&d,4),ot=(uint32_t)unit_wire_read(&d,4);
   if(ot==XR_XIR_UNIT&&om!=owner){unit_wire_attack(bytes,size,copy,ops[o].at+24,8,other,XR_XIR_BAD_STRUCTURE,false);++attacks;break;}
  }
 }
 CHECK(unit_writes>=2&&unit_reads&&attacks>=10);
 unit_wire_attack(bytes,size,copy,12,4,49,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,12,4,48,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,8,4,18,XR_XIR_BAD_STRUCTURE,false);
 unit_wire_attack(bytes,size,copy,20,4,1,XR_XIR_BAD_STRUCTURE,false);
 XrXirArtifact *artifact=NULL;size_t live=runtime_live,physical=runtime_bytes;
 CHECK(xr_xir_checked_read(bytes,size-1,NULL,&artifact,NULL)==XR_XIR_BAD_STRUCTURE&&!artifact);
 CHECK(runtime_live==live&&runtime_bytes==physical);
 free(copy);free(ops);free(owners);printf("Unit slot %u independently located field attacks and old revision/reserved/truncation PASS\n",attacks);
}
#endif
