/* Distinct transaction families share the real allocation/handle observer. */
#define wmain update_observer_wmain
#include "test_cli_toolchain_update.c"
#undef wmain

static void matrix_file(const char *path,const char *data,bool write){
    XrOsIoPolicy policy=xr_os_io_system_policy();wchar_t *wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,path,&wide)==XR_OS_IO_OK);
    HANDLE file=CreateFileW(wide,write?GENERIC_WRITE:GENERIC_READ,FILE_SHARE_READ,NULL,
        write?CREATE_ALWAYS:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    policy.free(policy.context,wide);CHECK(file!=INVALID_HANDLE_VALUE);
    size_t length=strlen(data);DWORD count=0;
    if(write)CHECK(WriteFile(file,data,(DWORD)length,&count,NULL)&&count==length);
    else{char bytes[8192];CHECK(length<sizeof(bytes));
        CHECK(ReadFile(file,bytes,(DWORD)length+1,&count,NULL)&&count==length);CHECK(!memcmp(bytes,data,length));}
    CHECK(CloseHandle(file));
}
static void matrix_remove(const char *path,bool directory){
    XrOsIoPolicy policy=xr_os_io_system_policy();wchar_t *wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,path,&wide)==XR_OS_IO_OK);
    BOOL removed=directory?RemoveDirectoryW(wide):DeleteFileW(wide);DWORD error=GetLastError();
    policy.free(policy.context,wide);
    CHECK(removed||error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND);
}
static bool matrix_missing(const char *path){
    XrOsIoPolicy policy=xr_os_io_system_policy();wchar_t *wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,path,&wide)==XR_OS_IO_OK);
    DWORD attributes=GetFileAttributesW(wide),error=GetLastError();policy.free(policy.context,wide);
    return attributes==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND);
}
typedef struct MatrixCase {
    XrCliToolchainUpdateRequest request;
    const char *path,*initial;
    char nested[3][4096],file[4096],lock[4096],large[8193],many[4096];
    XrCliToolchainUpdateStatus expected,construction_status;
    unsigned phase;
} MatrixCase;
static void matrix_case(MatrixCase *test,const char *path,unsigned phase){
    memset(test,0,sizeof(*test));test->phase=phase;test->path=path;test->initial=original;
    test->request=(XrCliToolchainUpdateRequest){path,"native","C:/after/cl.exe",NULL,XR_TOOLCHAIN_SELECTOR_MSVC,false};
    switch(phase){
        case 0:break;
        case 1:test->request.target="aarch64-windows-msvc";break;
        case 2:test->request.reset=true;test->request.selector=XR_TOOLCHAIN_SELECTOR_AUTO;test->request.compiler=NULL;break;
        case 3:test->request.reset=true;test->request.selector=XR_TOOLCHAIN_SELECTOR_AUTO;test->request.compiler=NULL;
            test->request.target=NULL;test->initial="broken = [\n";break;
        case 4:
            CHECK(snprintf(test->nested[0],sizeof(test->nested[0]),"%s.nested",path)>0);
            CHECK(snprintf(test->nested[1],sizeof(test->nested[1]),"%s/用户",test->nested[0])>0);
            CHECK(snprintf(test->nested[2],sizeof(test->nested[2]),"%s/deep",test->nested[1])>0);
            CHECK(snprintf(test->file,sizeof(test->file),"%s/toolchains.toml",test->nested[2])>0);
            CHECK(snprintf(test->lock,sizeof(test->lock),"%s.lock",test->file)>0);
            test->path=test->file;test->request.path=test->file;test->initial=NULL;break;
        case 5:test->request.path=NULL;break;
        case 6:test->initial="broken = [\n";test->expected=XR_CLI_TOOLCHAIN_UPDATE_INVALID;break;
        case 7:memset(test->large,'a',4096);test->large[4096]=0;test->request.compiler=test->large;break;
        case 8:test->request.target="not-a-real-target";
            test->construction_status=test->expected=XR_CLI_TOOLCHAIN_UPDATE_INVALID;break;
        case 9:test->request.compiler="C:/bad/\xff.exe";
            test->construction_status=test->expected=XR_CLI_TOOLCHAIN_UPDATE_INVALID;break;
        case 10:test->request.selector=XR_TOOLCHAIN_SELECTOR_ZIG;test->request.compiler=NULL;test->request.zig="C:/owned/zig.exe";break;
        case 11:test->request.selector=XR_TOOLCHAIN_SELECTOR_AUTO;test->request.zig="C:/owned/zig.exe";break;
        case 12:{
            static const char *const targets[]={"aarch64-apple-darwin","x86_64-apple-darwin","x86_64-linux-gnu",
                "aarch64-linux-gnu","i386-linux-musl","x86_64-linux-musl","arm-linux-gnueabi","aarch64-linux-musl",
                "powerpc64-linux-musl","powerpc64le-linux-musl","loongarch64-linux-musl","x86_64-windows-msvc",
                "aarch64-windows-msvc","x86_64-windows-gnu","aarch64-windows-gnu","riscv64-freestanding-none"};
            size_t used=sizeof(original)-1;memcpy(test->many,original,used+1);
            for(size_t i=0;i<16;++i){int n=snprintf(test->many+used,sizeof(test->many)-used,
                "\n[target.\"%s\"]\nprovider = \"msvc\"\ncompiler = \"old-%zu.exe\"\n",targets[i],i);
                CHECK(n>0&&(size_t)n<sizeof(test->many)-used);used+=(size_t)n;}
            test->initial=test->many;break;
        }
        case 13:memset(test->large,'a',sizeof(test->large)-1);test->large[sizeof(test->large)-1]=0;
            test->request.compiler=test->large;
            test->construction_status=test->expected=XR_CLI_TOOLCHAIN_UPDATE_INVALID;break;
        default:CHECK(false);
    }
}
static void matrix_setup(const MatrixCase *test){
    if(test->phase==4){
        matrix_remove(test->path,false);matrix_remove(test->lock,false);
        for(size_t i=3;i>0;--i)matrix_remove(test->nested[i-1],true);
    }else matrix_file(test->path,test->initial,true);
}
static void matrix_preserved(const MatrixCase *test){
    if(test->initial)matrix_file(test->path,test->initial,false);
    else CHECK(matrix_missing(test->path));
}
static int matrix_main(int argc,char **argv){
    CHECK(argc==4);winapi_control();
    unsigned phase=(unsigned)atoi(argv[2]);bool resources=!strcmp(argv[3],"resources");
    MatrixCase test;matrix_case(&test,argv[1],phase);matrix_setup(&test);
    XrCompileResourceStats stats={0};XrCliToolchainUpdateStatus construction=XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY;
    XrCliToolchainUpdateStatus baseline=run(&test.request,SIZE_MAX,&defaults,&stats,false,&construction);
    CHECK(construction==test.construction_status);
    if(baseline!=test.expected)fprintf(stderr,"phase=%u actual=%u expected=%u\n",phase,(unsigned)baseline,(unsigned)test.expected);
    CHECK(baseline==test.expected);
    size_t total=calls;
    if(resources){
        for(size_t cut=0;cut<total;++cut){
            matrix_setup(&test);XrCompileResourceStats unused={0};
            CHECK(run(&test.request,cut,&defaults,&unused,false,NULL)==XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY);
            matrix_preserved(&test);
        }
        const uint64_t bounds[3]={stats.allocated_bytes,stats.peak_bytes,stats.work};
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
            matrix_setup(&test);XrCompileResourceLimits limits=defaults;uint64_t bound=bounds[axis];CHECK(bound);
            uint64_t selected=delta<0?bound-1:bound+(unsigned)delta;
            if(axis==0)limits.allocated_bytes=selected;if(axis==1)limits.live_bytes=selected;if(axis==2)limits.work=selected;
            XrCompileResourceStats unused={0};XrCliToolchainUpdateStatus status=run(&test.request,SIZE_MAX,&limits,&unused,false,NULL);
            CHECK(status==(delta<0?XR_CLI_TOOLCHAIN_UPDATE_BUDGET:test.expected));
            if(delta<0||status!=XR_CLI_TOOLCHAIN_UPDATE_OK)matrix_preserved(&test);
        }
    }
    printf("phase=%u observed-allocations=%zu swept=%zu axes=%u status=%u physical=0 handles=baseline\n",
        phase,total,resources?total:0,resources?3u:0u,(unsigned)test.expected);
    return 0;
}
int wmain(int argc,wchar_t **wide){
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=matrix_main(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
