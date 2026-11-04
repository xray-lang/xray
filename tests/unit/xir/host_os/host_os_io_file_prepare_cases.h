/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_prepare_cases.h - Unsupported file calls with no host effects
 *
 * KEY CONCEPT:
 *   Admission and real execution are checked independently. An unimplemented
 *   primitive must fault, leave its result empty and consume no standard input.
 */
#ifndef HOST_OS_IO_FILE_PREPARE_CASES_H
#define HOST_OS_IO_FILE_PREPARE_CASES_H
#if !defined(XR_OS_WINDOWS)
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
extern char **environ;
#endif
enum { HOST_FILE_ENTRY,HOST_FILE_MAKE,HOST_FILE_CLOSE,HOST_FILE_FLUSH,HOST_FILE_OPEN,
 HOST_FILE_OPEN_WRITE,HOST_FILE_READ,HOST_FILE_WRITE,HOST_FILE_WRITE_STR,HOST_FILE_COUNT };
static bool host_file_prepare_child;
static const char *host_file_prepare_executable;
static const unsigned char host_file_prepare_bytes[]={17,13,10,26,0,255,42};
static const unsigned char host_file_prepare_output[]={161,0,13,10,26,255,17};
static const unsigned char host_file_prepare_error[]={177,0,255,42};
#if defined(XR_HOST_OS_FILE_PREPARE_SOURCE)
static const char host_file_prepare_text[]=
 "import {Path} from path\n"
 "class Child extends Path{tail:string\ninit(raw:string){super(raw);this.tail=\"file-prepare-tail\"}}\n"
 "export fn make(raw:string)->Path{return Child(raw)}\n"
 "export fn closeFile(handle:i64)->bool{return __fileClose(handle)}\n"
 "export fn flushFile(handle:i64)->bool{return __fileFlush(handle)}\n"
 "export fn openFile(path:Path)->i64{return __fileOpen(path)}\n"
 "export fn openWrite(path:Path,append:bool)->i64{return __fileOpenWrite(path,append)}\n"
 "export fn readFile(handle:i64,maxBytes:i64)->Array<u8>?{return __fileRead(handle,maxBytes)}\n"
 "export fn writeFile(handle:i64,data:Array<u8>,offset:i64)->i64{return __fileWrite(handle,data,offset)}\n"
 "export fn writeText(handle:i64,data:string,offset:i64)->i64{return __fileWriteStr(handle,data,offset)}\n"
 "fn refOpen(path:ref Path)->i64{return __fileOpen(path)}\n"
 "export fn byRef(raw:string)->i64{var path:Path=Child(raw);return refOpen(ref path)}\n"
 "export fn closeInDefer(handle:i64){defer{__fileClose(handle)}}\n"
 "export fn ctfeClose()->bool{return __fileClose(3)}\n"
 "export fn ctfeFlush()->bool{return __fileFlush(1)}\n"
 "export fn ctfeOpen()->i64{return __fileOpen(Child(\".\"))}\n"
 "export fn ctfeOpenWrite()->i64{return __fileOpenWrite(Child(\".\"),false)}\n"
 "export fn ctfeRead()->Array<u8>?{return __fileRead(0,7)}\n"
 "export fn ctfeWrite()->i64{const data:Array<u8>=[0 as u8,255 as u8];return __fileWrite(1,data,0)}\n"
 "export fn ctfeWriteStr()->i64{return __fileWriteStr(2,\"owned\",0)}\n";
#endif
static const unsigned char host_file_prepare_string[]={65,13,10,26,0,0xE4,0xB8,0xAD};
typedef struct HostFilePrepareFixture {char root[4096],file[8192],absent[8192];} HostFilePrepareFixture;
static void host_file_prepare_fixture(HostFilePrepareFixture *fixture,bool create){
#if defined(XR_OS_WINDOWS)
 wchar_t root[4096],file[8192],absent[8192];
 if(create){
  DWORD length=GetTempPathW(3000,root);CHECK(length&&length<3000);
  CHECK(swprintf_s(root+length,4096-length,L"xray-file-prepare-%lu-\x4e2d",GetCurrentProcessId())>0&&CreateDirectoryW(root,NULL));
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,root,-1,fixture->root,sizeof(fixture->root),NULL,NULL)>0);
  CHECK(snprintf(fixture->file,sizeof(fixture->file),"%s/protected.bin",fixture->root)>0);
  CHECK(snprintf(fixture->absent,sizeof(fixture->absent),"%s/absent.bin",fixture->root)>0);
 }else CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->root,-1,root,4096)>0);
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->file,-1,file,8192)>0);
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->absent,-1,absent,8192)>0);
 CHECK(GetFileAttributesW(absent)==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND);
 HANDLE handle=CreateFileW(file,create?GENERIC_WRITE:GENERIC_READ,0,NULL,create?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
 CHECK(handle!=INVALID_HANDLE_VALUE);DWORD count=0;unsigned char actual[sizeof(host_file_prepare_bytes)+1]={0};
 if(create)CHECK(WriteFile(handle,host_file_prepare_bytes,(DWORD)sizeof(host_file_prepare_bytes),&count,NULL)&&count==sizeof(host_file_prepare_bytes));
 else CHECK(ReadFile(handle,actual,(DWORD)sizeof(actual),&count,NULL)&&count==sizeof(host_file_prepare_bytes)&&!memcmp(actual,host_file_prepare_bytes,count));
 CHECK(CloseHandle(handle));if(!create)CHECK(DeleteFileW(file)&&RemoveDirectoryW(root));
#else
 if(create){
  memcpy(fixture->root,"/tmp/xray-file-prepare-XXXXXX",sizeof("/tmp/xray-file-prepare-XXXXXX"));CHECK(mkdtemp(fixture->root));
  CHECK(snprintf(fixture->file,sizeof(fixture->file),"%s/protected-\xE4\xB8\xAD.bin",fixture->root)>0);
  CHECK(snprintf(fixture->absent,sizeof(fixture->absent),"%s/absent.bin",fixture->root)>0);
 }
 struct stat unused;CHECK(stat(fixture->absent,&unused)==-1&&errno==ENOENT);
 FILE *file=fopen(fixture->file,create?"wb":"rb");CHECK(file);unsigned char actual[sizeof(host_file_prepare_bytes)+1]={0};
 if(create)CHECK(fwrite(host_file_prepare_bytes,1,sizeof(host_file_prepare_bytes),file)==sizeof(host_file_prepare_bytes));
 else CHECK(fread(actual,1,sizeof(actual),file)==sizeof(host_file_prepare_bytes)&&!memcmp(actual,host_file_prepare_bytes,sizeof(host_file_prepare_bytes))&&!ferror(file));
 CHECK(!fclose(file));if(!create)CHECK(!unlink(fixture->file)&&!rmdir(fixture->root));
#endif
}
static void host_file_prepare_calls(XrXirProgram *program,const uint32_t ids[HOST_FILE_COUNT]){
 HostFilePrepareFixture fixture={0};host_file_prepare_fixture(&fixture,true);
 for(unsigned attempt=0;attempt<8;++attempt){
  unsigned operation=attempt==7?3u:attempt;
  size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_instance(program,ids[HOST_FILE_ENTRY]);
  XrXirValue arguments[3]={{XR_XIR_I64,0,operation==2||operation==3?3u:operation==4?0u:operation==6?2u:1u},{0},{0}};
  uint32_t count=1;XrXirValue object={0},data={0};
  if(operation==2||operation==3){
   const char *path=attempt==7?fixture.absent:fixture.file;
   object=host_io_object(instance,ids[HOST_FILE_MAKE],path,strlen(path));arguments[0]=object;
   if(operation==3){arguments[1]=(XrXirValue){XR_XIR_BOOL,0,0};count=2;}
  }else if(operation==4){arguments[1]=(XrXirValue){XR_XIR_I64,0,sizeof(host_file_prepare_bytes)};count=2;}
  else if(operation==5){
   XrXirValue elements[sizeof(host_file_prepare_bytes)]={0};
   for(unsigned n=0;n<sizeof(host_file_prepare_bytes);++n)elements[n]=(XrXirValue){XR_XIR_U8,0,host_file_prepare_bytes[n]};
   XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
   CHECK(xr_xir_array_new(program->entries[ids[HOST_FILE_WRITE]].parameters[1],elements,sizeof(host_file_prepare_bytes),&admission,&data)==XR_XIR_VALUE_OK);
   arguments[1]=data;arguments[2]=(XrXirValue){XR_XIR_I64,0,1};count=3;
  }else if(operation==6){
   CHECK(xr_xir_string_new(instance->domain,(const char *)host_file_prepare_string,sizeof(host_file_prepare_string),&data)==XR_XIR_VALUE_OK);
   arguments[1]=data;arguments[2]=(XrXirValue){XR_XIR_I64,0,1};count=3;
  }
  uint32_t native=UINT32_MAX;
  for(uint32_t f=0;f<program->entry_count;++f)if(program->declarations->functions[f].native_callable==111u+operation)native=f;
  CHECK(native!=UINT32_MAX&&xr_xir_instance_start(instance,native,arguments,count)==XR_XIR_CALL_BAD_ARGUMENT);
  host_io_phase="file preparation Unsupported";
  host_io_failure(instance,ids[HOST_FILE_CLOSE+operation],arguments,count,XR_XIR_CALL_UNSUPPORTED);
  if(data.type==XR_XIR_STRING){const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(&data,&text,&length)&&length==sizeof(host_file_prepare_string)&&!memcmp(text,host_file_prepare_string,length));}
  xr_xir_value_drop(&data);xr_xir_value_drop(&object);
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_UNSUPPORTED&&runtime_live==live&&runtime_bytes==bytes);
 }
 host_file_prepare_fixture(&fixture,false);
}
static void host_file_prepare_stdio(void){
 unsigned char actual[sizeof(host_file_prepare_bytes)+1]={0};
 CHECK(fread(actual,1,sizeof(actual),stdin)==sizeof(host_file_prepare_bytes)&&!ferror(stdin)&&
  !memcmp(actual,host_file_prepare_bytes,sizeof(host_file_prepare_bytes)));
 CHECK(fwrite(host_file_prepare_output,1,sizeof(host_file_prepare_output),stdout)==sizeof(host_file_prepare_output)&&!fflush(stdout));
 CHECK(fwrite(host_file_prepare_error,1,sizeof(host_file_prepare_error),stderr)==sizeof(host_file_prepare_error)&&!fflush(stderr));
}
static void host_file_prepare_isolated(void){
#if defined(XR_OS_WINDOWS)
 SECURITY_ATTRIBUTES inherit={sizeof(inherit),NULL,TRUE};HANDLE input[2],output[2],error[2];
 CHECK(CreatePipe(input,input+1,&inherit,0)&&CreatePipe(output,output+1,&inherit,0)&&CreatePipe(error,error+1,&inherit,0));
 CHECK(SetHandleInformation(input[1],HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(output[0],HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(error[0],HANDLE_FLAG_INHERIT,0));
 wchar_t executable[32768],command[32768];DWORD length=GetModuleFileNameW(NULL,executable,32768);CHECK(length&&length<32768);
 CHECK(swprintf_s(command,32768,L"\"%ls\" --file-prepare-stdio",executable)>0);
 HANDLE job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit={0};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
 CHECK(job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit)));
 STARTUPINFOW start={0};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;start.hStdInput=input[0];start.hStdOutput=output[1];start.hStdError=error[1];PROCESS_INFORMATION process={0};
 CHECK(CreateProcessW(executable,command,NULL,NULL,TRUE,CREATE_SUSPENDED,NULL,NULL,&start,&process));
 if(!AssignProcessToJobObject(job,process.hProcess)){
  CHECK(TerminateProcess(process.hProcess,1)&&WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0);
  CHECK(CloseHandle(process.hThread)&&CloseHandle(process.hProcess)&&CloseHandle(job));CHECK(false);
 }
 CHECK(CloseHandle(input[0])&&CloseHandle(output[1])&&CloseHandle(error[1]));
 DWORD count=0;CHECK(WriteFile(input[1],host_file_prepare_bytes,(DWORD)sizeof(host_file_prepare_bytes),&count,NULL)&&count==sizeof(host_file_prepare_bytes)&&CloseHandle(input[1]));
 CHECK(ResumeThread(process.hThread)!=(DWORD)-1&&CloseHandle(process.hThread));
 DWORD waited=WaitForSingleObject(process.hProcess,30000);if(waited!=WAIT_OBJECT_0){CHECK(TerminateJobObject(job,1)&&WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0);}
 DWORD code=1;CHECK(GetExitCodeProcess(process.hProcess,&code));unsigned char observed[512];
 BOOL read=ReadFile(output[0],observed,sizeof(observed),&count,NULL);DWORD output_count=count;
 CHECK((read||GetLastError()==ERROR_BROKEN_PIPE)&&output_count==sizeof(host_file_prepare_output)&&!memcmp(observed,host_file_prepare_output,output_count));
 read=ReadFile(error[0],observed,sizeof(observed),&count,NULL);CHECK(read||GetLastError()==ERROR_BROKEN_PIPE);
 if(count!=sizeof(host_file_prepare_error)||memcmp(observed,host_file_prepare_error,sizeof(host_file_prepare_error))){
  fprintf(stderr,"file preparation child exit%lu stderr%lu: ",code,count);CHECK(fwrite(observed,1,count,stderr)==count);
 }
 CHECK(count==sizeof(host_file_prepare_error)&&!memcmp(observed,host_file_prepare_error,count));
 CHECK(CloseHandle(output[0])&&CloseHandle(error[0])&&CloseHandle(process.hProcess)&&CloseHandle(job)&&waited==WAIT_OBJECT_0&&!code);
#else
 int input[2],output[2],error[2];CHECK(!pipe(input)&&!pipe(output)&&!pipe(error));
 int descriptors[]={input[0],input[1],output[0],output[1],error[0],error[1]};
 for(unsigned n=0;n<6;++n)CHECK(descriptors[n]>2&&fcntl(descriptors[n],F_SETFD,FD_CLOEXEC)!=-1);
 posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));
 CHECK(!posix_spawn_file_actions_adddup2(&actions,input[0],0)&&!posix_spawn_file_actions_adddup2(&actions,output[1],1)&&!posix_spawn_file_actions_adddup2(&actions,error[1],2));
 for(unsigned n=0;n<6;++n)CHECK(!posix_spawn_file_actions_addclose(&actions,descriptors[n]));
 size_t sent=0;while(sent<sizeof(host_file_prepare_bytes)){ssize_t written=write(input[1],host_file_prepare_bytes+sent,sizeof(host_file_prepare_bytes)-sent);if(written<0&&errno==EINTR)continue;CHECK(written>0);sent+=(size_t)written;}
 char *arguments[]={(char *)host_file_prepare_executable,"--file-prepare-stdio",NULL};pid_t child=0;
 CHECK(!posix_spawnp(&child,host_file_prepare_executable,&actions,NULL,arguments,environ)&&!posix_spawn_file_actions_destroy(&actions));
 CHECK(!close(input[0])&&!close(input[1])&&!close(output[1])&&!close(error[1]));
 struct timespec beginning,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&beginning));int status=0;pid_t done=0;
 do{CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));done=waitpid(child,&status,WNOHANG);if(done<0&&errno==EINTR)continue;CHECK(done>=0);if(done)break;
  struct timespec pause={0,1000000};(void)nanosleep(&pause,NULL);
 }while(now.tv_sec-beginning.tv_sec<30);
 if(!done){CHECK(!kill(child,SIGKILL));while(waitpid(child,&status,0)<0)CHECK(errno==EINTR);}
 unsigned char observed[512];ssize_t count=read(output[0],observed,sizeof(observed));
 CHECK(count==(ssize_t)sizeof(host_file_prepare_output)&&!memcmp(observed,host_file_prepare_output,sizeof(host_file_prepare_output)));
 count=read(error[0],observed,sizeof(observed));CHECK(count==(ssize_t)sizeof(host_file_prepare_error)&&!memcmp(observed,host_file_prepare_error,sizeof(host_file_prepare_error)));
 CHECK(read(output[0],observed,1)==0&&read(error[0],observed,1)==0&&!close(output[0])&&!close(error[0]));
 CHECK(done==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
#endif
}
static void host_file_prepare_execute(XrXirProgram *program,const uint32_t ids[HOST_FILE_COUNT]){
 host_file_prepare_calls(program,ids);
 if(host_file_prepare_child)host_file_prepare_stdio();else host_file_prepare_isolated();
}
#endif // HOST_OS_IO_FILE_PREPARE_CASES_H
