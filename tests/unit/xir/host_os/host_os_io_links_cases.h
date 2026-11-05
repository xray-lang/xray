/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_links_cases.h - Actual link text through owned native results
 *
 * KEY CONCEPT:
 *   A mount-point query proves its actual tag, never a symbolic-link success.
 *   Platform reference results and retained values remain separate facts.
 */
#ifndef HOST_OS_IO_LINKS_CASES_H
#define HOST_OS_IO_LINKS_CASES_H
static void host_links_text(const XrXirValue *value,const char *expected){
 bool some=false;const XrXirValue *text=NULL;CHECK(xr_xir_nullable_view(value,&some,&text));
 if(!expected){CHECK(!some&&!text);return;}
 const char *bytes=NULL;size_t length=0;
 CHECK(some&&text&&xr_xir_string_view(text,&bytes,&length)&&length==strlen(expected)&&!memcmp(bytes,expected,length));
}
static void host_io_links_execute(XrXirProgram *program,const uint32_t ids[6]){
 size_t live=runtime_live,bytes=runtime_bytes;char root[4096],file[4096],link[4096],trial[4096],reference_name[4096],reference[4096],target[4096];
 bool expected_symlink=false;
#if defined(XR_OS_WINDOWS)
 wchar_t wide_root[1024],wide_file[1024],wide_link[1024],wide_trial[1024],wide_reference[1024];
 DWORD length=GetTempPathW(900,wide_root);CHECK(length&&length<900);
 CHECK(swprintf_s(wide_root+length,1024-length,L"xray-links-%lu-\x6d4b\x8bd5",GetCurrentProcessId())>0&&CreateDirectoryW(wide_root,NULL));
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_root,-1,root,sizeof(root),NULL,NULL)>0);
#else
 memcpy(root,"/tmp/xray-io-links-XXXXXX",sizeof("/tmp/xray-io-links-XXXXXX"));CHECK(mkdtemp(root));
#endif
 CHECK(snprintf(file,sizeof(file),"%s/source-\xe4\xb8\xad.bin",root)>0&&snprintf(link,sizeof(link),"%s/mount",root)>0);
 CHECK(snprintf(trial,sizeof(trial),"%s/trial",root)>0&&snprintf(reference_name,sizeof(reference_name),"%s/reference",root)>0);
 host_io_write(file);
#if defined(XR_OS_WINDOWS)
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,file,-1,wide_file,1024)>0);
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,link,-1,wide_link,1024)>0&&CreateDirectoryW(wide_link,NULL));
 CHECK(host_io_junction(wide_link,wide_root));
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,trial,-1,wide_trial,1024)>0);
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reference_name,-1,wide_reference,1024)>0);
 HANDLE handle=CreateFileW(wide_file,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
 CHECK(handle!=INVALID_HANDLE_VALUE);wchar_t final[2048];
 DWORD count=GetFinalPathNameByHandleW(handle,final,2048,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
 CHECK(count&&count<2048&&CloseHandle(handle));const wchar_t *ordinary=final;
 /* This fixture creates ordinary names; the independent query keeps casing. */
 if(count>=7&&!wmemcmp(final,L"\\\\?\\",4)&&final[5]==L':'&&final[6]==L'\\')ordinary=final+4;
 else {CHECK(count>=10&&!wmemcmp(final,L"\\\\?\\UNC\\",8));final[6]=L'\\';ordinary=final+6;}
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,ordinary,-1,reference,sizeof(reference),NULL,NULL)>0);
 CHECK(snprintf(target,sizeof(target),"\\\\?\\%s",root)>0);
 expected_symlink=CreateSymbolicLinkW(wide_reference,wide_file,SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)!=0;
 if(!expected_symlink){DWORD error=GetLastError();fprintf(stderr,"symlink Win32 reference refused=%lu\n",(unsigned long)error);CHECK(error==ERROR_PRIVILEGE_NOT_HELD);}
#else
 CHECK(!symlink(root,link));
 CHECK(realpath(file,reference)==reference&&snprintf(target,sizeof(target),"%s",root)>0);
 expected_symlink=symlink(file,reference_name)==0;CHECK(expected_symlink);
#endif
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;links_mode=LINKS_NORMAL;
 XrXirValue object=host_io_object(instance,ids[1],file,strlen(file)),mount=host_io_object(instance,ids[1],link,strlen(link));
 XrXirValue destination=host_io_object(instance,ids[1],trial,strlen(trial)),args[]={object,destination};
 for(uint32_t id=XR_NATIVE_CALLABLE_IO_SYMLINK;id<=XR_NATIVE_CALLABLE_IO_REALPATH;++id){
  uint32_t function=UINT32_MAX;for(uint32_t n=0;n<program->entry_count;++n)
   if(program->declarations->functions[n].native_callable==id)function=n;
  unsigned before=links_calls[id-XR_NATIVE_CALLABLE_IO_SYMLINK];
  CHECK(function!=UINT32_MAX&&xr_xir_instance_start(instance,function,args,id==XR_NATIVE_CALLABLE_IO_SYMLINK?2u:1u)==XR_XIR_CALL_BAD_ARGUMENT);
  CHECK(links_calls[id-XR_NATIVE_CALLABLE_IO_SYMLINK]==before);
 }
 XrXirValue changed=host_invoke(instance,ids[2],args,2);CHECK(changed.type==XR_XIR_BOOL&&changed.payload==expected_symlink);xr_xir_value_drop(&changed);
 XrXirValue canonical=host_invoke(instance,ids[4],&object,1),substitute=host_invoke(instance,ids[3],&mount,1);
 host_links_text(&canonical,reference);host_links_text(&substitute,target);
 XrXirValue none=host_invoke(instance,ids[3],&object,1);host_links_text(&none,NULL);xr_xir_value_drop(&none);
 XrXirValue raw={0};CHECK(xr_xir_string_new(instance->domain,file,strlen(file),&raw)==XR_XIR_VALUE_OK);
 XrXirValue by_ref=host_invoke(instance,ids[5],&raw,1);host_links_text(&by_ref,reference);xr_xir_value_drop(&raw);xr_xir_value_drop(&by_ref);
 xr_xir_value_drop(&object);xr_xir_value_drop(&mount);xr_xir_value_drop(&destination);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 host_links_text(&canonical,reference);host_links_text(&substitute,target);xr_xir_value_drop(&canonical);xr_xir_value_drop(&substitute);
#if defined(XR_OS_WINDOWS)
 /* Provider-text cases traverse the complete managed call, not a mirror helper. */
 const struct {const char *raw,*expected;} final_texts[]={
  {"\\\\?\\C:\\","C:\\"},
  {"\\\\?\\C:\\\347\233\256\345\275\225 \347\251\272\351\227\264\\\346\225\260\346\215\256.bin","C:\\\347\233\256\345\275\225 \347\251\272\351\227\264\\\346\225\260\346\215\256.bin"},
  {"\\\\?\\UNC\\server\\share\\\344\270\255\346\226\207.bin","\\\\server\\share\\\344\270\255\346\226\207.bin"},
  {"\\\\?\\UNC\\host.example\\C$\\a","\\\\host.example\\C$\\a"},
  {"\\\\?\\UNC\\127.0.0.1\\share","\\\\127.0.0.1\\share"},
  {"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\file","\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\file"},
  {"\\\\?\\Volume{1234}\\file","\\\\?\\Volume{1234}\\file"},
  {"\\\\.\\PhysicalDrive0","\\\\.\\PhysicalDrive0"},
  {"\\??\\C:\\file","\\??\\C:\\file"},
  {"\\\\?\\C:relative","\\\\?\\C:relative"},
  {"\\\\?\\UNC\\server","\\\\?\\UNC\\server"},
  {"\\\\?\\UNC\\server\\","\\\\?\\UNC\\server\\"},
  {"\\\\?\\UNC\\server\\.","\\\\?\\UNC\\server\\."},
  {"\\\\?\\UNC\\server\\..","\\\\?\\UNC\\server\\.."},
  {"\\\\?\\C:\\trailing.","\\\\?\\C:\\trailing."},
  {"\\\\?\\C:\\trailing ","\\\\?\\C:\\trailing "},
  {"\\\\?\\C:\\NUL.txt","\\\\?\\C:\\NUL.txt"},
  {"\\\\?\\C:\\COM\302\271","\\\\?\\C:\\COM\302\271"},
  {"\\\\?\\C:\\dir\\..\\file","\\\\?\\C:\\dir\\..\\file"},
 };
 for(size_t n=0;n<sizeof(final_texts)/sizeof(final_texts[0]);++n){
  instance=host_instance(program,ids[0]);host_active=instance;
  object=host_io_object(instance,ids[1],file,strlen(file));links_final_override=final_texts[n].raw;
  unsigned before=links_calls[2];canonical=host_invoke(instance,ids[4],&object,1);links_final_override=NULL;
  CHECK(links_calls[2]==before+1);xr_xir_value_drop(&object);
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
  host_links_text(&canonical,final_texts[n].expected);xr_xir_value_drop(&canonical);
 }
#endif
 for(unsigned path_index=0;path_index<2;++path_index){
  instance=host_instance(program,ids[0]);host_active=instance;
  object=host_io_object(instance,ids[1],file,strlen(file));const char nul[]={'x',0,'y'};
  destination=host_io_object(instance,ids[1],nul,sizeof(nul));args[0]=path_index?object:destination;args[1]=path_index?destination:object;
  unsigned before=links_calls[0];host_io_failure(instance,ids[2],args,2,XR_XIR_CALL_HOST_ERROR);CHECK(links_calls[0]==before);
  xr_xir_value_drop(&object);xr_xir_value_drop(&destination);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR);host_active=NULL;
 }
 for(unsigned operation=1;operation<3;++operation){
  for(unsigned mode=LINKS_OOM;mode<=LINKS_RETAIN_CLOSE;++mode){
#if !defined(XR_OS_WINDOWS)
   if(mode==LINKS_RETAIN_CLOSE)continue;
#endif
   instance=host_instance(program,ids[0]);host_active=instance;links_mode=LINKS_NORMAL;
   const char *path=operation==1?link:file;object=host_io_object(instance,ids[1],path,strlen(path));links_mode=mode;
   XrXirCallStatus expected=mode==LINKS_OOM||mode==LINKS_RESULT_OOM?XR_XIR_CALL_OOM:
    mode==LINKS_RETAIN_CLOSE?XR_XIR_CALL_HOST_ERROR:XR_XIR_CALL_CANCELLED;
   unsigned before=links_calls[operation];host_io_failure(instance,ids[operation+2],&object,1,expected);runtime_fail_at=SIZE_MAX;
   CHECK(links_calls[operation]==before+1);xr_xir_value_drop(&object);
   if(mode==LINKS_RETAIN_CLOSE){
    CHECK(instance->file_metadata_owners);stat_close_failures=1;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED&&instance->file_metadata_owners);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR&&links_calls[operation]==before+1);
   }else CHECK(xr_xir_instance_free(instance)==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:expected));
   host_active=NULL;
  }
 }
 links_mode=LINKS_NORMAL;
#if defined(XR_OS_WINDOWS)
 if(expected_symlink)CHECK(DeleteFileW(wide_reference)&&DeleteFileW(wide_trial));
 CHECK(RemoveDirectoryW(wide_link)&&DeleteFileW(wide_file)&&RemoveDirectoryW(wide_root));
#else
 CHECK(!unlink(reference_name)&&!unlink(trial)&&!unlink(link)&&!unlink(file)&&!rmdir(root));
#endif
 CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_IO_LINKS_CASES_H
