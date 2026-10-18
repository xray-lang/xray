/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_regex_private_source.h - Exact private ordinary bodies for bounded unit tests
 */
#ifndef XIR_REGEX_PRIVATE_SOURCE_H
#define XIR_REGEX_PRIVATE_SOURCE_H
static char *regex_private_source(const char *const *names,size_t count,size_t *length) {
 CHECK(count && count<=8);FILE *file=fopen(XR_REGEX_FULL_SOURCE,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0 && size<1048576 && !fseek(file,0,SEEK_SET));
 char *bytes=xr_malloc((size_t)size+1);CHECK(bytes && fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));bytes[size]=0;
 const char *start[8]={0};size_t span[8]={0};*length=0;
 for(size_t i=0;i<count;++i){start[i]=strstr(bytes,names[i]);CHECK(start[i] && !strstr(start[i]+1,names[i]));const char *end=strstr(start[i],"\n}");CHECK(end && (end[2]=='\n' || (end[2]=='\r' && end[3]=='\n')));span[i]=(size_t)(end+(end[2]=='\r'?4:3)-start[i]);CHECK(span[i]<65536-*length);*length+=span[i];}
 char *source=xr_malloc(*length+1);CHECK(source);size_t offset=0;for(size_t i=0;i<count;++i){memcpy(source+offset,start[i],span[i]);offset+=span[i];}source[*length]=0;memset(bytes,0xa5,(size_t)size);xr_free(bytes);return source;
}
#endif // XIR_REGEX_PRIVATE_SOURCE_H
