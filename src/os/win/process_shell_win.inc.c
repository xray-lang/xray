/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_shell_win.inc.c - Explicit system cmd transport without CRT escaping
 */
static wchar_t *owned_process_system_shell(ProcBuild *text) {
    if(!proc_text_current(text))return NULL;
    wchar_t probe[1];UINT needed=GetSystemDirectoryW(probe,1);
    if(!needed){text->status=proc_text_last_error();return NULL;}
    static const wchar_t suffix[]=L"\\cmd.exe";
    if((size_t)needed>SIZE_MAX/sizeof(wchar_t)-sizeof(suffix)/sizeof(*suffix)) {
        text->status=XR_PROC_OVERFLOW;return NULL;
    }
    size_t capacity=(size_t)needed+sizeof(suffix)/sizeof(*suffix);
    wchar_t *program=proc_alloc(text,capacity*sizeof(*program));if(!program)return NULL;
    UINT length=GetSystemDirectoryW(program,needed);
    if(!length||length>=needed) {
        text->status=length?XR_PROC_IO:proc_text_last_error();proc_free(text,program);return NULL;
    }
    size_t at=length;
    if(program[at-1]==L'\\'||program[at-1]==L'/')--at;
    proc_text_copy(text,program+at,suffix,sizeof(suffix));
    if(!proc_text_current(text)||!proc_exact_program(program)) {
        if(text->status==XR_PROC_OK)text->status=XR_PROC_IO;
        proc_free(text,program);return NULL;
    }
    return program;
}
static wchar_t *owned_process_shell_command(ProcBuild *text,const char *command,wchar_t **program,bool *refused) {
    *program=owned_process_system_shell(text);if(!*program)return NULL;
    wchar_t *raw=proc_wide(text,command);if(!raw)return NULL;
    size_t executable=0,length=0;wchar_t *output=NULL;
    if(!proc_wlength(text,*program,&executable)||!proc_wlength(text,raw,&length))goto done;
    static const wchar_t middle[]=L"\" /D /S /C \"";
    size_t separators=sizeof(middle)/sizeof(*middle)-1;
    if(executable>SIZE_MAX-separators-3||length>SIZE_MAX-executable-separators-3||
        executable+separators+length+3>SIZE_MAX/sizeof(*output)) {text->status=XR_PROC_OVERFLOW;goto done;}
    size_t capacity=executable+separators+length+3;
    /* The Win32 command-line bound includes prefix, both outer quotes and
     * terminator. cmd may independently reject its own smaller shell limits. */
    if(capacity>32767){*refused=true;goto done;}
    output=proc_alloc(text,capacity*sizeof(*output));if(!output)goto done;
    size_t at=0;proc_emit(text,output,&at,L'"');
    proc_text_copy(text,output+at,*program,executable*sizeof(*output));at+=executable;
    proc_text_copy(text,output+at,middle,separators*sizeof(*output));at+=separators;
    /* cmd /S strips only these outer quotes. Interior command text is shell
     * syntax and must never pass through the ordinary CRT argument encoder. */
    proc_text_copy(text,output+at,raw,length*sizeof(*output));at+=length;
    proc_emit(text,output,&at,L'"');proc_emit(text,output,&at,0);
    if(text->status!=XR_PROC_OK){proc_free(text,output);output=NULL;}
done:
    proc_free(text,raw);return output;
}
