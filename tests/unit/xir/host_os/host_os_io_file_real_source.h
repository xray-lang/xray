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
static const char host_file_real_exports[]=
 "\nexport fn filePath(raw:string)->Path{return Path(raw)}\n"
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
 "export fn fileWriteTextRaw(handle:i64,data:string,offset:i64)->i64{return __fileWriteStr(handle,data,offset)}\n";
static XrXirArtifact *host_file_real_code(XrXirCompileContext *context){
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
 CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
 CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return code;
}
static void host_file_real_product(XrXirCompileContext *context,const char *directory,bool emit){
 if(!emit&&!host_file_stdio_child)host_file_contracts(context);
 XrXirArtifact *code=host_file_real_code(context);const XrXirModule *module=xr_xir_compile_artifact_module(code);
 uint32_t ids[HOST_REAL_COUNT]={module->declarations->entry_function};
 static const char *const names[]={"filePath","fileRoundtrip","fileDeferred","fileStdio","fileOpenRaw","fileReadRaw","fileCloseRaw","fileFlushRaw","fileWriteRaw","fileWriteTextRaw"};
 for(unsigned n=0;n<HOST_REAL_COUNT-1;++n)ids[n+1]=host_named(module,names[n]);
 host_emit(code,"host_io_file_real",ids,HOST_REAL_COUNT,directory,emit);
 if(emit)xr_xir_compile_artifact_free(code);
 else{XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK&&!code);if(host_file_stdio_child)host_file_stdio_execute(program,ids);else host_file_real_execute(program,ids);xr_xir_compile_program_drop(program);}
}
#endif // HOST_OS_IO_FILE_REAL_SOURCE_H
