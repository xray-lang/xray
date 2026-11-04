/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_real_cases.h - Public File bytes and independently closed files
 *
 * KEY CONCEPT:
 *   Real public File calls must preserve explicit byte offsets and caller data.
 *   Windows exclusive opens verify closure without inspecting a token registry.
 */
#ifndef HOST_OS_IO_FILE_REAL_CASES_H
#define HOST_OS_IO_FILE_REAL_CASES_H
#include "os/os_time.h"
static const unsigned char host_file_real_input[]={0x41,0x0D,0x0A,0x1A,0,0xFF,0x42};
/* The two-byte UTF-8 prefix makes offset two distinct from a rune index. */
static const unsigned char host_file_real_text[]={0xC3,0xA9,0,0xE4,0xB8,0xAD};
static const unsigned char host_file_real_expected[]={0x0D,0x0A,0x1A,0,0xFF,0x42,0,0xE4,0xB8,0xAD};
typedef struct HostFileRealFixture {char root[4096],path[8192];} HostFileRealFixture;
static void host_file_real_fixture(HostFileRealFixture *fixture,unsigned phase){
 static const unsigned char previous[]="previous contents must be truncated";
#if defined(XR_OS_WINDOWS)
 wchar_t root[4096],path[8192];
 if(!phase){
  DWORD length=GetTempPathW(3000,root);CHECK(length&&length<3000);
  CHECK(swprintf_s(root+length,4096-length,L"xray-file-real-%lu-\x4e2d",GetCurrentProcessId())>0&&CreateDirectoryW(root,NULL));
  CHECK(swprintf_s(path,8192,L"%ls\\\x6587\x4ef6.bin",root)>0);
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,root,-1,fixture->root,sizeof(fixture->root),NULL,NULL)>0);
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,fixture->path,sizeof(fixture->path),NULL,NULL)>0);
 }else{
  CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->root,-1,root,4096)>0);
  CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->path,-1,path,8192)>0);
 }
 if(phase==2){CHECK(DeleteFileW(path)&&RemoveDirectoryW(root));return;}
 /* Share mode zero would reject an outstanding File read or write handle. */
 HANDLE file=CreateFileW(path,phase?GENERIC_READ:GENERIC_WRITE,0,NULL,phase?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 CHECK(file!=INVALID_HANDLE_VALUE);DWORD count=0;
 if(!phase)CHECK(WriteFile(file,previous,(DWORD)(sizeof(previous)-1),&count,NULL)&&count==sizeof(previous)-1);
 else{
  unsigned char actual[sizeof(host_file_real_expected)+1]={0};
  CHECK(ReadFile(file,actual,(DWORD)sizeof(actual),&count,NULL)&&count==sizeof(host_file_real_expected)&&
   !memcmp(actual,host_file_real_expected,sizeof(host_file_real_expected)));
 }
 CHECK(CloseHandle(file));
#else
 if(!phase){
  memcpy(fixture->root,"/tmp/xray-file-real-XXXXXX",sizeof("/tmp/xray-file-real-XXXXXX"));CHECK(mkdtemp(fixture->root));
  CHECK(snprintf(fixture->path,sizeof(fixture->path),"%s/\xE6\x96\x87\xE4\xBB\xB6.bin",fixture->root)>0);
 }
 if(phase==2){CHECK(!unlink(fixture->path)&&!rmdir(fixture->root));return;}
 /* This byte oracle does not claim Windows share-mode closure coverage. */
 FILE *file=fopen(fixture->path,phase?"rb":"wb");CHECK(file);
 if(!phase)CHECK(fwrite(previous,1,sizeof(previous)-1,file)==sizeof(previous)-1);
 else{unsigned char actual[sizeof(host_file_real_expected)+1]={0};
  CHECK(fread(actual,1,sizeof(actual),file)==sizeof(host_file_real_expected)&&!ferror(file)&&
   !memcmp(actual,host_file_real_expected,sizeof(host_file_real_expected)));}
 CHECK(!fclose(file));
#endif
}
static XrXirValue host_file_real_invoke(XrXirInstance *instance,uint32_t function,const XrXirValue *arguments,uint32_t count){
 CHECK(xr_xir_instance_start(instance,function,arguments,count)==XR_XIR_CALL_READY);
 uint64_t beginning=xr_time_monotonic_ns();CHECK(beginning);XrXirCallStatus status=XR_XIR_CALL_READY;
 for(;;){
  XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);status=event.outcome.status;
  if(status==XR_XIR_CALL_SUSPENDED){
   XrXirWaitRequest wait={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&wait)==XR_XIR_CALL_READY);
   CHECK(wait.kind==XR_XIR_WAIT_YIELD&&xr_xir_instance_resume(instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);xr_thread_yield();
  }else if(status!=XR_XIR_CALL_READY)break;
  uint64_t now=xr_time_monotonic_ns();CHECK(now&&now>=beginning&&now-beginning<UINT64_C(30000000000));
 }
 if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"real File function%u status%u\n",function,status);
 CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_RETURNED);return output;
}
static void host_file_real_array(const XrXirValue *value,const unsigned char *expected,size_t count,XrXirValueAdmission *admission){
 int64_t length=-1;CHECK(xr_xir_array_len(value,admission,&length)==XR_XIR_VALUE_OK&&length==(int64_t)count);
 for(size_t n=0;n<count;++n){XrXirValue element={0};XrXirFaultDetail fault={0};
  CHECK(xr_xir_array_get(value,(int64_t)n,admission,&element,&fault)==XR_XIR_VALUE_OK&&element.type==XR_XIR_U8&&element.payload==expected[n]);
  xr_xir_value_drop(&element);}
}
static void host_file_real_execute(XrXirProgram *program,const uint32_t ids[4]){
 size_t live=runtime_live,bytes=runtime_bytes;HostFileRealFixture fixture={0};host_file_real_fixture(&fixture,0);
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValue initialized=host_file_real_invoke(instance,ids[0],NULL,0);CHECK(initialized.type==XR_XIR_I64&&!initialized.payload);xr_xir_value_drop(&initialized);
 XrXirValue raw={0},input={0},text={0};CHECK(xr_xir_string_new(instance->domain,fixture.path,strlen(fixture.path),&raw)==XR_XIR_VALUE_OK);
 XrXirValue path=host_file_real_invoke(instance,ids[1],&raw,1);xr_xir_value_drop(&raw);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 XrXirValue elements[sizeof(host_file_real_input)]={0};
 for(unsigned n=0;n<sizeof(host_file_real_input);++n)elements[n]=(XrXirValue){XR_XIR_U8,0,host_file_real_input[n]};
 CHECK(xr_xir_array_new(program->entries[ids[2]].parameters[1],elements,sizeof(host_file_real_input),&admission,&input)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_string_new(instance->domain,(const char *)host_file_real_text,sizeof(host_file_real_text),&text)==XR_XIR_VALUE_OK);
 XrXirValue arguments[]={path,input,text};XrXirValue result=host_file_real_invoke(instance,ids[2],arguments,3);
 host_file_real_array(&result,host_file_real_expected,sizeof(host_file_real_expected),&admission);
 host_file_real_array(&input,host_file_real_input,sizeof(host_file_real_input),&admission);
 const char *actual=NULL;size_t length=0;CHECK(xr_xir_string_view(&text,&actual,&length)&&length==sizeof(host_file_real_text)&&!memcmp(actual,host_file_real_text,length));
 xr_xir_value_drop(&result);host_file_real_fixture(&fixture,1);
 result=host_file_real_invoke(instance,ids[3],&path,1);CHECK(result.type==XR_XIR_I64&&result.payload==13);xr_xir_value_drop(&result);
 host_file_real_fixture(&fixture,1);xr_xir_value_drop(&path);xr_xir_value_drop(&input);xr_xir_value_drop(&text);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);host_file_real_fixture(&fixture,2);
}
#endif // HOST_OS_IO_FILE_REAL_CASES_H
