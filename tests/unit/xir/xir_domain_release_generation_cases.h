/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_domain_release_generation_cases.h - Always-on rejection before retired-owner release
 */
#ifndef XIR_DOMAIN_RELEASE_GENERATION_CASES_H
#define XIR_DOMAIN_RELEASE_GENERATION_CASES_H
static int prefix_domain_release_child(int argc, char **argv) {
    if (argc == 1) return -1;
    if (argc != 2 || strlen(argv[1]) != 15 || memcmp(argv[1],"--domain-void-",14) ||
        argv[1][14] < '0' || argv[1][14] > '6') return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SYSTEM_INFO info; GetSystemInfo(&info); size_t page = info.dwPageSize;
    unsigned char *memory = VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(page >= 8 && page <= SIZE_MAX/2 && memory); DWORD previous = 0;
    CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&previous));
    unsigned char *prefix = memory+page-8; memset(prefix,0,8);
    atomic_init((_Atomic(uint32_t) *)prefix,1); atomic_init((atomic_bool *)(prefix+4),false);
    XrXirDomain *domain = (XrXirDomain *)prefix;
    switch (argv[1][14]) {
    case '0': xr_xir_domain_drop(domain); break;
    case '1': xr_xir_domain_close(domain); break;
    case '2': xr_xir_domain_deallocate(domain,(void *)(uintptr_t)1,1); break;
    case '3': xr_xir_domain_metadata_deallocate(domain,(void *)(uintptr_t)1,1); break;
    case '4': xr_xir_domain_call_deallocate(domain,(void *)(uintptr_t)1,1); break;
    case '5': {
        XrXirCallBudget budget = {.work_domain=domain};
        xr_xir_call_budget_deallocate(&budget,(void *)(uintptr_t)1,1); break;
    }
    case '6': {
        XrXirClassObservation observation = {.domain=domain};
        XrXirClassObservation *pointer = &observation;
        xr_xir_class_observation_drop(&pointer); break;
    }
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
    return 4;
}
static void prefix_domain_release_case(unsigned mode) {
    wchar_t executable[2048], command[2112];
    DWORD length = GetModuleFileNameW(NULL,executable,2048); CHECK(length && length < 2048);
    int written = _snwprintf_s(command,2112,_TRUNCATE,L"\"%ls\" --domain-void-%u",executable,mode);
    CHECK(written > 0 && written < 2112);
    SECURITY_ATTRIBUTES security = {sizeof(security),NULL,TRUE}; HANDLE reader = NULL, writer = NULL;
    CHECK(CreatePipe(&reader,&writer,&security,0) && SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0));
    STARTUPINFOW startup = {0}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdError = writer; startup.hStdOutput = writer;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {0};
    CHECK(CreateProcessW(executable,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
    CHECK(CloseHandle(writer));
    CHECK(WaitForSingleObject(process.hProcess,10000) == WAIT_OBJECT_0);
    DWORD status = 0; CHECK(GetExitCodeProcess(process.hProcess,&status) && status == 3);
    char message[4096] = {0}; DWORD bytes = 0;
    CHECK(ReadFile(reader,message,sizeof(message)-1,&bytes,NULL) && bytes && bytes < sizeof(message));
    const char *const expected[] = {
        "domain drop requires its actual storage generation",
        "domain close cannot consume a retired owner",
        "domain lock requires the current immutable prefix",
        "execution storage needs its current domain owner",
        "execution storage needs its current domain owner",
        "executor release requires a current domain before accounting",
        "observation release retains its current owner"};
    CHECK(mode < sizeof(expected)/sizeof(expected[0]));
    if (!strstr(message,"[FATAL]") || !strstr(message,expected[mode]))
        fprintf(stderr,"Domain release mode=%u actual exit=%lu message=%s\n",mode,(unsigned long)status,message);
    CHECK(strstr(message,"[FATAL]") && strstr(message,expected[mode]));
    CHECK(CloseHandle(reader) && CloseHandle(process.hThread) && CloseHandle(process.hProcess));
}
static void prefix_domain_release_cases(void) {
    xr_xir_domain_drop(NULL); xr_xir_domain_close(NULL);
    for (unsigned mode=0;mode<7;++mode) prefix_domain_release_case(mode);
}
#endif // XIR_DOMAIN_RELEASE_GENERATION_CASES_H
