/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_thread_token_contracts.h - Actual library bodies and private syscall authority
 */
#ifndef HOST_OS_THREAD_TOKEN_CONTRACTS_H
#define HOST_OS_THREAD_TOKEN_CONTRACTS_H
static const char host_thread_token_tail[]=
 "enum InitErr{Failed}\n"
 "export fn current()->i64{return __threadLocalId()}\n"
 "export fn alive(id:i64)->bool{return __threadLocalAlive(id)}\n"
 "export fn throughValue()->i64{var f=current;return f()}\n"
 "export fn basic()->i64{var calls=0\nconst local=ThreadLocal<i64,Never>(fn()->i64{calls=calls+1;return 42})\n"
 "assert(try! local.get()==42);assert(try! local.get()==42);assert(calls==1);local.set(7);assert(try! local.get()==7);return 42}\n"
 "export fn retry()->i64{var calls=0\nconst local=ThreadLocal<i64,InitErr>(fn()throws(InitErr)->i64{calls=calls+1;if(calls==1){throw InitErr.Failed}\nreturn 42})\n"
 "var failed=false\ntry{var unused=try local.get()}catch(e){failed=true}\nassert(failed);assert(try! local.get()==42);assert(calls==2);return 42}\n"
 "var recursive:ThreadLocal<i64,ThreadLocalError>?=null\n"
 "export fn recursion()->i64{recursive=ThreadLocal<i64,ThreadLocalError>(fn()throws(ThreadLocalError)->i64{return try recursive!.get()})\n"
 "var failed=false\ntry{var unused=try recursive!.get()}catch(e){failed=true}\nassert(failed);recursive!.set(7);assert(try! recursive!.get()==7);return 42}\n"
 "export fn panicked()->i64{var first=true\nvar calls=0\nconst local=ThreadLocal<i64,Never>(fn()->i64{calls=calls+1;if(first){first=false;assert(false)}\nreturn 42})\n"
 "var failed=false\ntry{var unused=try! local.get()}catch panic(p){failed=true}\nassert(failed);assert(try! local.get()==42);assert(calls==2);return 42}\n"
 "export fn cancelled()throws(ThreadLocalError)->i64{var key=__threadLocalId()\n"
 "const local=ThreadLocal<i64,Never>(fn()->i64{var unused=__threadLocalAlive(0);return 42})\n"
 "defer{assert(!local._initializing.containsKey(key))}\nreturn try local.get()}\n"
 "export fn yielded()->i64{const local=ThreadLocal<i64,Never>(fn()->i64{return 42})\nvar result=try! local.get();Coro.yield();return result}\n"
 "fn child()->i64{return 7}\n"
 "export fn waited()->i64{const local=ThreadLocal<i64,Never>(fn()->i64{return 42})\nvar result=try! local.get()\nvar task=go child()\nvar unused=await task\nreturn result}\n"
 "export fn dynamic()->i64{var init:nosuspend fn()->i64=fn()->i64{return 42}\nconst local=ThreadLocal<i64,Never>(init);return try! local.get()}\n"
 "export fn nullable()->i64{var calls=0\nconst local=ThreadLocal<i64?,Never>(fn()->i64?{calls=calls+1;return null})\n"
 "assert(try! local.get()==null);assert(try! local.get()==null);assert(calls==1);return 42}\n";

/* Only these exact source-owned bodies are selected. Unsupported unrelated
 * sys features are not a claim of complete module admission. */
static char *host_thread_token_source(void){
 FILE *file=fopen(XR_HOST_OS_ROOT "/stdlib/sys/sys.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));
 long length=ftell(file);CHECK(length>=0&&(uint64_t)length<SIZE_MAX&&!fseek(file,0,SEEK_SET));
 char *whole=xr_malloc((size_t)length+1);CHECK(whole&&fread(whole,1,(size_t)length,file)==(size_t)length&&!fclose(file));whole[length]=0;
 const char *spin=strstr(whole,"fn _spinAcquire("),*spin_end=strstr(whole,"// A negative state denotes a writer");
 const char *local=strstr(whole,"export enum ThreadLocalError {"),*local_end=strstr(whole,"export class Dylib {");
 CHECK(spin&&spin_end&&spin<spin_end&&local&&local_end&&local<local_end);
 size_t a=(size_t)(spin_end-spin),b=(size_t)(local_end-local),tail=sizeof(host_thread_token_tail);
 CHECK(a<=SIZE_MAX-b&&a+b<=SIZE_MAX-tail);char *text=xr_malloc(a+b+tail);CHECK(text);
 memcpy(text,spin,a);memcpy(text+a,local,b);memcpy(text+a+b,host_thread_token_tail,tail);xr_free(whole);return text;
}
static void host_thread_token_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *m=xr_xir_compile_artifact_module(code);const char *names[]={"__threadLocalId","__threadLocalAlive"};
 for(unsigned n=0;n<2;++n){
  uint32_t f=host_named(m,names[n]);CHECK(xir_native_callable_verify(m,f)==XR_XIR_OK);
  CHECK(m->functions[f].result==(n?XR_XIR_BOOL:XR_XIR_I64)&&m->functions[f].parameter_count==n);
  CHECK(m->declarations->functions[f].promises==XR_XIR_FUNCTION_NO_SUSPEND);
  XrXirValue argument={XR_XIR_I64,0,0};host_ctfe(code,f,n?&argument:NULL,n);
  XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};char text[128];
  CHECK(snprintf(text,sizeof(text),"%s(%s)",names[n],n?"0":"")>0);
  CHECK(host_check(context,"sys",text,false,&output,&diagnostic)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);
  xr_xir_compile_source_result_free(&output);
  CHECK(snprintf(text,sizeof(text),"export fn bad(){var value=%s}",names[n])>0);
  CHECK(host_check(context,"sys",text,true,&output,&diagnostic)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);
  CHECK(strstr(diagnostic.message,"direct call"));xr_xir_compile_source_result_free(&output);
  CHECK(snprintf(text,sizeof(text),"export fn bad(){%s(%s)}",names[n],n?"":"0")>0);
  CHECK(host_check(context,"sys",text,true,&output,&diagnostic)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);
  xr_xir_compile_source_result_free(&output);
 }
 const char *shadows[]={"fn __threadLocalId()->i64{return 7}","fn __threadLocalAlive(id:i64)->bool{return false}"};
 for(unsigned n=0;n<2;++n){
  XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};CHECK(host_check(context,"sys",shadows[n],true,&output,&diagnostic)==XR_XIR_OK);
  const XrXirModule *shadow=xr_xir_compile_artifact_module(output.checked);CHECK(!shadow->declarations->functions[host_named(shadow,names[n])].native_callable);
  xr_xir_compile_source_result_free(&output);
 }
 const char *invalid[]={
  "export fn bad(){var local=ThreadLocal<i64,Never>(fn()->i64{Coro.yield();return 42})}",
  "export fn bad(init:fn()->i64){var local=ThreadLocal<i64,Never>(init)}",
  "export fn bad(){var local=ThreadLocal<i64,Never>(42)}",
  "export fn bad(){var local=ThreadLocal<i64,Never>(fn()->i64{return 42});local._init=fn()->i64{Coro.yield();return 42}}",
  "enum InitErr{Failed}\nexport fn bad(){var local=ThreadLocal<i64,Never>(fn()throws(InitErr)->i64{throw InitErr.Failed})}"};
 char *source=host_thread_token_source();char *tail=strstr(source,"enum InitErr{Failed}");CHECK(tail);size_t prefix=(size_t)(tail-source);
 for(unsigned n=0;n<sizeof(invalid)/sizeof(*invalid);++n){
  size_t length=strlen(invalid[n]);CHECK(prefix<=SIZE_MAX-length-1);char *text=xr_malloc(prefix+length+1);CHECK(text);
  memcpy(text,source,prefix);memcpy(text+prefix,invalid[n],length+1);XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
  CHECK(host_check(context,"sys",text,true,&output,&diagnostic)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);
  xr_xir_compile_source_result_free(&output);xr_free(text);
 }
 xr_free(source);
}
#endif // HOST_OS_THREAD_TOKEN_CONTRACTS_H
