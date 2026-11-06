#include "os/os_fs.h"
/* Instrument actual current command/write sites; no replay or loader proof. */
static unsigned invocation_runs,invocation_process_fail=UINT_MAX;
static unsigned owner_writes,owner_tamper_write=UINT_MAX;
static XrOsIoStatus owner_write(const XrOsIoPolicy *policy,const char *path,const void *bytes,size_t length) {
    XrOsIoStatus status=xr_os_io_write_new_file_sync(policy,path,bytes,length);
    if(owner_writes++==owner_tamper_write && status==XR_OS_IO_OK) {
        CHECK(length);FILE *file=fopen(path,"r+b");CHECK(file);unsigned char changed=((const unsigned char *)bytes)[0]^1u;
        CHECK(fwrite(&changed,1,1,file)==1 && !fclose(file));
    }
    return status;
}
static XrProcessStatus owner_run(const XrToolchainProcess *process,XrProcessCancelled cancelled,void *context,XrProcessResult *output) {
    unsigned stage=invocation_runs++;
    if(stage==invocation_process_fail)runtime_fail_at=runtime_attempts;
    XrProcessStatus status=xtc_process_run(process,cancelled,context,output);runtime_fail_at=SIZE_MAX;return status;
}
#define xtc_process_run owner_run
#define xr_os_io_write_new_file_sync owner_write
#include "app/toolchain/xtc_xir_invocation.c"
#undef xr_os_io_write_new_file_sync
#undef xtc_process_run
