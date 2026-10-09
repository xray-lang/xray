/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_typed_programs.h
 */
#ifndef XIR_LIBRARY_TYPED_PROGRAMS_H
#define XIR_LIBRARY_TYPED_PROGRAMS_H

static const char library_typed_definition[]=
    "export struct Payload{const text:string;const n:i64}\n"
    "struct Secret{const text:string}\n"
    "export struct Locked{private text:string}\n"
    "export interface Measure{measure()->i64}\n"
    "interface Hidden{hidden()->i64}\n"
    "fn secret()->i64{return 99;}\n"
    "export fn make()->Payload{return Payload{text:\"typed\",n:41};}\n"
    "export fn number(value:Payload)->i64{return value.n;}\n"
    "export fn text(value:Payload)->string{return value.text;}\n"
;
static const char library_typed_consumer_0[]=
    "import \"./typed_closed\" as lib;\n"
    "struct Local{value:i64}\n"
    "export fn result()->i64{const p=lib.make();if(lib.text(p)!=\"typed\"){return 0;};const q=lib.Payload{text:\"local\",n:lib.number(p)};if(q.text!=\"local\"){return 0;};return q.n;}\n"
;
static const char library_typed_consumer_1[]=
    "import \"./typed_closed\" as lib;\n"
    "struct Local implements lib.Measure{const text:string;const n:i64;measure()->i64{return this.n;}}\n"
    "fn use<T:lib.Measure>(value:T)->i64{return value.measure();}\n"
    "export fn result()->i64{const local=Local{text:\"witness\",n:41};return use<Local>(local);}\n"
;
static const char *const library_typed_consumers[]={library_typed_consumer_0,library_typed_consumer_1};
static const char library_construction_definition[]=
    "export struct Auto{const text:string=\"owned\";const n:i64=41;}\n"
    "export struct Required{const text:string;const n:i64;}\n"
    "export struct Locked{private text:string=\"secret\";const n:i64=41;}\n"
    "export struct Explicit{const n:i64;constructor(){this.n=41;}}\n"
    "export struct PrivateCtor{const n:i64;private constructor(){this.n=41;}}\n"
;
static const char library_construction_consumer[]=
    "import \"./construction\" as lib;\n"
    "struct Holder{const value:lib.Auto;}\n"
    "export fn result()->i64{const direct=lib.Explicit();if(direct.n!=41){return 0;};const a=lib.Auto();const b=lib.Auto{n:41};const h=Holder();if(a.text!=\"owned\"){return 0;};if(b.text!=\"owned\"){return 0;};if(h.value.text!=\"owned\"){return 0;};return h.value.n;}\n"
;
static const char library_generic_parent_definition_0[]=
    "export struct Box<T:AtomicNumber>{const value:T;const text:string;}\n"
    "export interface Base<T>{measure(value:T)->T}\n"
    "export interface Left<A> extends Base<A>{}\n"
    "export interface Right<B> extends Base<B>{}\n"
    "interface Private<T>{hidden(value:T)->T}\n"
    "export interface Mapper<T>{map<U:Equal>(value:U)->U}\n"
    "export interface Diamond<C> extends Left<C>,Right<C>{}\n"
    "export fn make<T:AtomicNumber>(value:T)->Box<T>{return Box<T>{value:value,text:\"owned\"};}\n"
    "export fn read<T:Diamond<i64>>(value:T)->i64{return value.measure(41);}\n"
;
static const char library_generic_parent_definition_1[]=
    "export struct Box<T:AtomicNumber>{const value:T;const text:string;}\n"
    "export interface Base<T>{measure(value:T)->T}\n"
    "export interface Left<A> extends Base<A>{}\n"
    "export interface Right<B> extends Base<B>{}\n"
    "interface Private<T>{hidden(value:T)->T}\n"
    "export interface Mapper<T>{map<U:Equal>(value:U)->U}\n"
    "export interface Diamond<C> extends Right<C>,Left<C>{}\n"
    "export fn make<T:AtomicNumber>(value:T)->Box<T>{return Box<T>{value:value,text:\"owned\"};}\n"
    "export fn read<T:Diamond<i64>>(value:T)->i64{return value.measure(41);}\n"
;
static const char library_generic_parent_consumer[]=
    "import \"./generic_parent\" as lib;\n"
    "interface LocalFirst{unused()->i64}\n"
    "struct Meter implements lib.Diamond<i64>{measure(value:i64)->i64{return value;}}\n"
    "struct Copier implements lib.Mapper<i64>{map<U:Equal>(value:U)->U{return value;}}\n"
    "fn copied<T:lib.Mapper<i64>>(value:T)->i64{return value.map<i64>(41);}\n"
    "export fn result()->i64{const box=lib.make<i64>(41);if(box.text!=\"owned\"){return 0;};if(box.value!=41){return 0;};if(copied<Copier>(Copier{})!=41){return 0;};return lib.read<Meter>(Meter{});}\n"
;
static const char *const library_generic_parent_definitions[]={library_generic_parent_definition_0,library_generic_parent_definition_1};
static const char library_member_witness_definition[]=
    "export interface Base<A>{read()->A}\n"
    "export interface Child<T> extends Base<T>{}\n"
    "export struct Box<T:Equal> implements Child<T>{const value:T;const text:string;read()->T{return this.value;}choose<U:Equal>(value:U)->U{return value;}static echo<U:Equal>(value:U)->U{return value;}private secret()->i64{return 99;}}\n"
    "export fn make()->Box<i64>{return Box<i64>{value:41,text:\"owned\"};}\n"
    "export fn extract<V:Child<i64>>(value:V)->i64{return value.read();}\n"
;
static const char library_member_witness_consumer[]=
    "import \"./member_witness\" as lib;\n"
    "interface LocalFirst{unused()->i64}\n"
    "struct Local{const text:string;}\n"
    "export fn result()->i64{const box=lib.make();if(box.text!=\"owned\"){return 0;};if(box.choose<string>(\"member\")!=\"member\"){return 0;};if(lib.Box<i64>.echo<i64>(41)!=41){return 0;};if(box.read()!=41){return 0;};return lib.extract<lib.Box<i64>>(box);}\n"
;
static const char library_carrier_member_definition[]=
    "export struct Envelope<T>{const entries:Array<(T,string?)>;const text:string;first()->T{return this.entries[0].0;}}\n"
    "export fn make<T>(value:T)->Envelope<T>{const text:string?=\"owned\";const item:(T,string?)=(value,text);return Envelope<T>{entries:[item],text:\"owned\"};}\n"
    "export struct Counter{n:i64;constructor(value:i64){this.n=value;}ref bump(){this.n=this.n+1;}read()->i64{return this.n;}}\n"
    "export struct Dispatch{const invoke:fn(i64)->i64;run(value:i64)->i64{return (this.invoke)(value);}}\n"
    "fn next(value:i64)->i64{return value+1;}\n"
    "export fn increment()->fn(i64)->i64{return next;}\n"
    "export fn apply(f:fn(i64)->i64,value:i64)->i64{return f(value);}\n"
    "export struct Secret{private const text:string;constructor(text:string){this.text=text;}}\n"
;
static const char library_carrier_member_consumer[]=
    "import \"./carrier_member\" as lib;\n"
    "struct Local{const prefix:Array<string>;}\n"
    "export fn result()->i64{const envelope=lib.make<i64>(41);if(envelope.text!=\"owned\"){return 0;};if(envelope.entries[0].1==null){return 0;};if(envelope.first()!=41){return 0;};var counter=lib.Counter(40);counter.bump();if(counter.read()!=41){return 0;};const dispatch=lib.Dispatch{invoke:lib.increment()};if(dispatch.run(40)!=41){return 0;};return lib.apply(lib.increment(),40);}\n"
;
static const char library_enum_owned_definition[]=
    "export enum Packet<T>{Empty,Value{item:T,text:string},Other{item:T} item(fallback:T)->T{return match(this){Packet.Empty->fallback,Packet.Value{item}->item,Packet.Other{item}->item};}}\n"
    "enum Hidden{Only}\n"
    "export fn make<T>(value:T)->Packet<T>{return Packet<T>.Value{item:value,text:\"owned\"};}\n"
    "export fn read(value:Packet<i64>)->i64{return match(value){Packet.Value{item,text}-> {if(text!=\"owned\"){return 0;};item},Packet.Empty->0,Packet.Other{item}->item};}\n"
;
static const char library_enum_owned_consumer[]=
    "import \"./enum_owned\" as lib;\n"
    "enum Local{Before,After{item:string}}\n"
    "export fn result()->i64{const packet=lib.make<i64>(41);if(packet.ordinal!=1){return 0;};if(packet.name!=\"Value\"){return 0;};if(packet.toString()!=\"Packet.Value\"){return 0;};if(packet.item(0)!=41){return 0;};const other=lib.Packet<i64>.Other{item:41};if(other.item(0)!=41){return 0;};return lib.read(packet);}\n"
;
static const char library_class_owned_definition[]=
    "export interface Read<T>{read()->T}\n"
    "export final class Box<T> implements Read<T>{private value:T;const text:string;constructor(value:T){this.value=value;this.text=\"owned\";}read()->T{return this.value;}set(value:T){this.value=value;}}\n"
    "export final class Secret{private value:i64;private constructor(value:i64){this.value=value;}static make()->Secret{return Secret(41);}read()->i64{return this.value;}}\n"
    "export final class Defaulted{value:i64=41;text:string=\"owned\";}\n"
    "export fn make<T>(value:T)->Box<T>{return Box<T>(value);}\n"
    "export fn through<V:Read<i64>>(value:V)->i64{return value.read();}\n"
;
static const char library_class_owned_consumer[]=
    "import \"./class_owned\" as lib;\n"
    "struct Local{const value:string;}\n"
    "export fn result()->i64{const box=lib.make<i64>(40);const alias=box;alias.set(41);if(box.read()!=41){return 0;};if(box.text!=\"owned\"){return 0;};if(lib.Secret.make().read()!=41){return 0;};const implicit=lib.Defaulted();if(implicit.value!=41){return 0;};if(implicit.text!=\"owned\"){return 0;};return lib.through<lib.Box<i64>>(box);}\n"
;
typedef struct LibraryTypedProgram {const char *name,*library_file,*library,*consumer;} LibraryTypedProgram;
static const LibraryTypedProgram library_typed_programs[]={
    {"typed_nominal","typed_closed.xr",library_typed_definition,library_typed_consumer_0},
    {"typed_interface","typed_closed.xr",library_typed_definition,library_typed_consumer_1},
    {"construction","construction.xr",library_construction_definition,library_construction_consumer},
    {"generic_parent_lr","generic_parent.xr",library_generic_parent_definition_0,library_generic_parent_consumer},
    {"generic_parent_rl","generic_parent.xr",library_generic_parent_definition_1,library_generic_parent_consumer},
    {"member_witness","member_witness.xr",library_member_witness_definition,library_member_witness_consumer},
    {"carrier_member","carrier_member.xr",library_carrier_member_definition,library_carrier_member_consumer},
    {"enum_owned","enum_owned.xr",library_enum_owned_definition,library_enum_owned_consumer},
    {"class_owned","class_owned.xr",library_class_owned_definition,library_class_owned_consumer},
};
static inline const LibraryTypedProgram *library_typed_program(unsigned index) {
    return &library_typed_programs[index];
}
#define LIBRARY_TYPED_PROGRAM_COUNT 9u
#endif /* XIR_LIBRARY_TYPED_PROGRAMS_H */
