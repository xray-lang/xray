/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_real_source.h - Complete io source and public File operations
 *
 * KEY CONCEPT:
 *   Only test exports are appended. Every original declaration, algorithm and
 *   imported module is checked before the producer and input packet retire.
 */
#ifndef HOST_OS_IO_FILE_REAL_SOURCE_H
#define HOST_OS_IO_FILE_REAL_SOURCE_H
#include "xir/xxir_class_stage_internal.h"
static const char host_file_real_exports[]=
 "\nimport {Buffer} from mem\nexport fn filePath(raw:string)->Path{return Path(raw)}\n"
 "export fn fileRoundtrip(path:Path,bytes:Array<u8>,text:string)->Array<u8>{\n"
 " var opened=File.openWrite(path,false);assert(opened!=null);const writer=opened!\n"
 " var offset:i64=1;while(offset<len(bytes)){const n=writer.write(bytes,offset);assert(n>0&&n<=len(bytes)-offset);offset+=n}\n"
 " assert(writer.flush());assert(writer.close());assert(writer.close())\n"
 " opened=File.openWrite(path,true);assert(opened!=null);const appended=opened!\n"
 " offset=2;while(offset<6){const n=appended.writeString(text,offset);assert(n>0&&n<=6-offset);offset+=n}\n"
 " assert(appended.flush());assert(appended.close())\n"
 " const readOpened=File.open(path);assert(readOpened!=null);const reader=readOpened!\n"
 " const zero=reader.read(0);assert(zero!=null&&len(zero!)==0);var out:Array<u8> = []\n"
 " while(true){const part=reader.read(3);assert(part!=null);if(len(part!)==0){break};assert(len(part!)<=3)\n"
 "  var i:i64=0;while(i<len(part!)){out.push(part![i]);i+=1}}\n"
 " const eof=reader.read(1);assert(eof!=null&&len(eof!)==0);assert(reader.close());return out}\n"
 "export fn fileDeferred(path:Path)->i64{const opened=File.open(path);assert(opened!=null);const file=opened!\n"
 " defer{assert(file.close())};const part=file.read(1);assert(part!=null&&len(part!)==1);return part![0] as i64}\n"
 "export fn fileStdio()->Array<u8>{const input=File.stdin();var bytes:Array<u8> = [13 as u8]\n"
 " const zero=input.read(0);assert(zero!=null&&len(zero!)==0)\n"
 " while(true){const part=input.read(3);assert(part!=null);if(len(part!)==0){break}\n"
 "  var i:i64=0;while(i<len(part!)){bytes.push(part![i]);i+=1}}\n"
 " assert(len(bytes)==10);assert(input.close());assert(input.close())\n"
 " const again=File.stdin().read(1);assert(again!=null&&len(again!)==0)\n"
 " const out=File.stdout();assert(out.writeString(\"A\",0)==1);print(\"B\")\n"
 " var offset:i64=0;while(offset<len(bytes)){const n=out.write(bytes,offset);assert(n>0);offset+=n}\n"
 " assert(out.flush());assert(out.close());assert(out.close());print(\"C\")\n"
 " const error=File.stderr();offset=0;while(offset<len(bytes)){const n=error.write(bytes,offset);assert(n>0);offset+=n}\n"
 " assert(error.flush());assert(error.close());assert(error.close())\n"
 " assert(!__fileClose(0));assert(!__fileClose(1));assert(!__fileClose(2));return bytes}\n"
 "export fn fileOpenRaw(path:Path)->i64{return __fileOpen(path)}\n"
 "export fn fileReadRaw(handle:i64,max:i64)->Array<u8>?{return __fileRead(handle,max)}\n"
 "export fn fileCloseRaw(handle:i64)->bool{return __fileClose(handle)}\n"
 "export fn fileFlushRaw(handle:i64)->bool{return __fileFlush(handle)}\n"
 "export fn fileWriteRaw(handle:i64,data:Array<u8>,offset:i64)->i64{return __fileWrite(handle,data,offset)}\n"
 "export fn fileWriteTextRaw(handle:i64,data:string,offset:i64)->i64{return __fileWriteStr(handle,data,offset)}\n"
 "export fn fileLossy(bytes:Array<u8>)->string{return string.fromUtf8Lossy(bytes[:])}\n"
 "export fn fileLossyCheck()->bool{const bytes:Array<u8> = [65 as u8,0 as u8,240 as u8,159 as u8,146 as u8]\n"
 " return string.fromUtf8Lossy(bytes[:])==\"A\\0\xEF\xBF\xBD\"}\n"
 "class FileConstructorStages{saved:Array<string>;first:i64\n"
 " init(handle:i64){this.saved=[\"staged\",\"\xE4\xB8\xAD\"];const bytes=__fileRead(handle,4)\n"
 " assert(bytes!=null&&len(bytes!)==4);this.first=bytes![0] as i64;assert(__fileClose(handle))}}\n"
 "export fn fileConstructorRaw(handle:i64)->i64{const value=FileConstructorStages(handle);assert(len(value.saved)==2);return value.first}\n"
 "export fn fileEager(path:Path)->i64{const reader=BufReader.open(path,2);assert(reader!=null)\n"
 " const iterator=LineIterator(reader!);const rest=reader!.readLine();assert(rest!=null)\n"
 " assert(iterator.hasNext());const first=iterator.next();assert(first!=null&&first! == \"\");assert(!iterator.hasNext());return len(rest!)}\n"
 "export fn fileEmptyIterator(path:Path)->LineIterator{const reader=BufReader.open(path,2);assert(reader!=null);return LineIterator(reader!)}\n"
 "final class FileFieldPlaces{data:Array<i64>;grid:Array<Array<i64>>;seen:i64\n"
 " init(input:Array<i64>){this.data=input;this.data.push(10);this.seen=len(this.data)\n"
 "  this.data.reserve(len(this.data=[7,8])+4);assert(this.data.capacity>=6)\n"
 "  this.grid=[[3]];var count:i64=0;this.grid[(count=count+1)-1].push(4);this.seen+=count}\n"
 " result()->Array<i64>{assert(this.seen==3);assert(len(this.grid[0])==2&&this.grid[0][1]==4);return this.data}}\n"
 "export fn fileFieldPlaces(input:Array<i64>)->Array<i64>{return FileFieldPlaces(input).result()}\n"
 "export fn fileBuffered(path:Path,input:Array<u8>)->Array<u8>{const writer=BufWriter(path,1)\n"
 " assert(writer.writeBytes(input)==len(input));const snapshot=writer.bytes()\n"
 " assert(writer.write(snapshot[:])==len(snapshot));var i:i64=0\n"
 " while(i<len(input)){assert(snapshot[i]==input[i]);i+=1}\n"
 " const text=\"A\\0\xE4\xB8\xAD\";assert(writer.write(text.bytes())==5)\n"
 " const storage=Buffer(3,true);assert(writer.write(storage.asBytes())==3);assert(storage.resize(4))\n"
 " assert(writer.writeString(text)==5);const result=writer.bytes();writer.reset();assert(writer.size()==0);return result}\n";
#include "host_os_io_file_ctor_contracts.h"
static XrXirArtifact *host_file_real_code(XrXirCompileContext *context,bool controls){
 FILE *file=fopen(XR_HOST_OS_ROOT "/stdlib/io/io.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0&&!fseek(file,0,SEEK_SET));
 CHECK((uint64_t)size<=SIZE_MAX-sizeof(host_file_real_exports));size_t length=(size_t)size+sizeof(host_file_real_exports)-1;
 char *text=xr_malloc(length+1);CHECK(text&&fread(text,1,(size_t)size,file)==(size_t)size&&!fclose(file));
 memcpy(text+(size_t)size,host_file_real_exports,sizeof(host_file_real_exports));
 const char *root=XR_HOST_OS_ROOT "/stdlib";XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"io",root};
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
 XrXirSourceRequest request={session,XR_HOST_OS_ROOT "/stdlib/io/io.xr",&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};
 XrXirSourceText input={"io/io.xr",text,length};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&source,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"complete io Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
 CHECK(status==XR_XIR_OK);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
 xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);memset(text,0xa5,length);xr_free(text);
 XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
 memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 if(controls)host_file_ctor_boundary_controls(context,cold);
 XrXirDiagnostic specialize_diagnostic={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
 status=xr_xir_compile_specialize(cold,&closed,&specialize_diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"complete io Specialize%u f%u b%u i%u kind%u\n",(unsigned)status,
  specialize_diagnostic.function,specialize_diagnostic.block,specialize_diagnostic.instruction,(unsigned)specialize_diagnostic.reason);
 CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
 CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return code;
}
static uint32_t host_file_constructor_contracts(const XrXirModule *module,uint32_t entry){
 uint32_t constructor=UINT32_MAX,helper=UINT32_MAX;const XrXirFunction *body=module->functions+entry;
 for(uint32_t i=0;i<body->instruction_count;++i){const XrXirInstruction *op=body->instructions+i;
  if(op->op==XR_XIR_CALL&&op->immediate>=0&&(uint64_t)op->immediate<module->function_count&&
   module->declarations->functions[op->immediate].method_kind==XR_XIR_CONSTRUCTOR)constructor=(uint32_t)op->immediate;}
 CHECK(constructor!=UINT32_MAX);uint32_t owner=module->declarations->functions[constructor].nominal_owner;
 for(uint32_t f=0;f<module->function_count;++f)if(module->declarations->functions[f].nominal_owner==owner&&
  module->declarations->functions[f].method_kind==XR_XIR_CONSTRUCTOR_FIELDS)helper=f;
 CHECK(helper!=UINT32_MAX&&!module->declarations->functions[constructor].promises&&!module->declarations->functions[helper].promises);
 CHECK(xir_compile_class_stage_verify(module,NULL)==XR_XIR_OK);
 CHECK(!module->function_count||sizeof(XrXirFunctionIdentity)<=SIZE_MAX/(size_t)module->function_count);
 XrXirFunctionIdentity *identities=xr_malloc(module->function_count*sizeof(*identities));CHECK(identities);
 memcpy(identities,module->declarations->functions,module->function_count*sizeof(*identities));
 XrXirDeclarations declarations=*module->declarations;declarations.functions=identities;XrXirModule changed=*module;changed.declarations=&declarations;
 CHECK(xir_compile_class_stage_verify(&changed,NULL)==XR_XIR_OK);
 identities[helper].promises=XR_XIR_FUNCTION_NO_SUSPEND;CHECK(xir_compile_class_stage_verify(&changed,NULL)==XR_XIR_BAD_TYPE);identities[helper].promises=0;
 identities[constructor].promises=XR_XIR_FUNCTION_NO_SUSPEND;CHECK(xir_compile_class_stage_verify(&changed,NULL)==XR_XIR_BAD_TYPE);
 identities[constructor].promises=identities[helper].promises=XR_XIR_FUNCTION_OVERRIDE;
 CHECK(xir_compile_class_stage_verify(&changed,NULL)==XR_XIR_BAD_TYPE);xr_free(identities);return helper;
}
static void host_file_real_product(XrXirCompileContext *context,const char *directory,bool emit){
 if(!emit&&!host_file_stdio_child)host_file_contracts(context);
 XrXirArtifact *code=host_file_real_code(context,!emit&&!host_file_stdio_child);const XrXirModule *module=xr_xir_compile_artifact_module(code);
 uint32_t ids[HOST_REAL_COUNT]={module->declarations->entry_function};
 static const char *const names[]={"filePath","fileRoundtrip","fileDeferred","fileStdio","fileOpenRaw","fileReadRaw","fileCloseRaw","fileFlushRaw","fileWriteRaw","fileWriteTextRaw","fileLossy","fileLossyCheck","fileConstructorRaw","fileEager","fileEmptyIterator","fileFieldPlaces","fileBuffered"};
 for(unsigned n=0;n<HOST_REAL_COUNT-2;++n)ids[n+1]=host_named(module,names[n]);
 ids[HOST_REAL_CTOR_HELPER]=host_file_constructor_contracts(module,ids[HOST_REAL_CONSTRUCTOR]);
 uint32_t decoder=host_named(module,"fromUtf8Lossy");CHECK(module->declarations->functions[decoder].native_callable==118u&&
  xir_native_callable_verify(module,decoder)==XR_XIR_OK);
 const XrXirFunction *head=module->functions+decoder;XrXirType wrong=XR_XIR_STRING;
 CHECK(!xir_native_callable_types(118u,module->types,head->parameters,1,XR_XIR_I64,XR_XIR_NEVER)&&
  !xir_native_callable_types(118u,module->types,head->parameters,1,XR_XIR_STRING,XR_XIR_UNIT)&&
  !xir_native_callable_types(118u,module->types,&wrong,1,XR_XIR_STRING,XR_XIR_NEVER));
 if(!emit&&!host_file_stdio_child){
  XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=ids[HOST_REAL_LOSSY_CHECK],
   .target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
   .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
  memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
  CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_OK&&result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);
 }
 host_emit(code,"host_io_file_real",ids,HOST_REAL_COUNT,directory,emit);
 if(emit)xr_xir_compile_artifact_free(code);
 else{XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK&&!code);if(host_file_stdio_child)host_file_stdio_execute(program,ids);else host_file_real_execute(program,ids);xr_xir_compile_program_drop(program);}
}
#endif // HOST_OS_IO_FILE_REAL_SOURCE_H
