/* Compiled with the authentic frozen Program28 headers. */
#include "xir/xxir_program.h"
#include "xir/xxir_nominal.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION==28 && XR_XIR_VALUE_ABI_VERSION==20 && XR_XIR_CALL_ABI_VERSION==25,"authentic prior ABI");
_Static_assert(sizeof(XrXirNominalDeclaration)==88 && sizeof(XrXirNominalIdentity)==72,"actual prior two strides");
extern const XrXirProgramSpec old28_program;
static unsigned callbacks,releases,leases;
static void *allocation;
static XrXirProgramSpec spec;
static XrXirTypes types;
static XrXirNominalTable table;
static XrXirCallEntry entries[2];
static XrXirAction poison_resume(XrXirCallView *view) {(void)view;++callbacks;return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);}
static void poison_release(XrXirCallView *view,XrXirCallStatus status) {(void)view;(void)status;++releases;}
static void poison_lease(void *owner) {CHECK(owner==&leases);++leases;}
const void *old28_guarded(unsigned declarations) {
    CHECK(!allocation && old28_program.types->nominals->count==2);
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    allocation=VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(allocation);
    DWORD protection=0;CHECK(VirtualProtect((char *)allocation+page,page,PAGE_NOACCESS,&protection));
    types=*old28_program.types;table=*types.nominals;types.nominals=&table;
    if (declarations) {
        XrXirNominalDeclaration rows[2]={{0}};
        for (uint32_t i=0;i<2;++i) {
            rows[i].module=table.identities[i].module;rows[i].name=table.identities[i].name;
            rows[i].exported=table.identities[i].exported;rows[i].kind=table.identities[i].kind;
        }
        XrXirNominalDeclaration *at=(XrXirNominalDeclaration *)((char *)allocation+page-sizeof(rows));
        memcpy(at,rows,sizeof(rows));table.declarations=at;table.identities=NULL;
    } else {
        XrXirNominalIdentity *at=(XrXirNominalIdentity *)((char *)allocation+page-sizeof(XrXirNominalIdentity[2]));
        memcpy(at,table.identities,sizeof(XrXirNominalIdentity[2]));table.identities=at;table.declarations=NULL;
    }
    memcpy(entries,old28_program.entries,sizeof(entries));
    for (uint32_t i=0;i<2;++i) {entries[i].resume=poison_resume;entries[i].release=poison_release;}
    spec=old28_program;spec.types=&types;spec.entries=entries;spec.code=(XrXirCodeLease){&leases,poison_lease};
    return &spec;
}
const void *old28_guard_page(void) {SYSTEM_INFO info;GetSystemInfo(&info);CHECK(allocation);return (char *)allocation+info.dwPageSize;}
unsigned old28_callback_count(void) {return callbacks;}
unsigned old28_release_count(void) {return releases;}
unsigned old28_lease_count(void) {return leases;}
void old28_guard_clear(void) {CHECK(allocation && VirtualFree(allocation,0,MEM_RELEASE));allocation=NULL;}
