/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Length-aware diagnostic quoting never truncates keys or treats NUL as EOF. */
#ifndef XR_LITERAL_QUOTE_CORE_H
#define XR_LITERAL_QUOTE_CORE_H
#include "xr_value_format_core.h"
XR_VALUE_FORMAT_FUNCTION int xr_literal_quote_bytes(XrValueFormatSink sink,const char *bytes,size_t length) {
    static const char hex[]="0123456789abcdef";
    if((length && !bytes) || !xr_value_format_write(sink,"\"",1))return 0;
    size_t start=0;
    for(size_t n=0;n<length;++n) {
        unsigned char byte=(unsigned char)bytes[n];
        if(byte>=32 && byte!='"' && byte!='\\' && byte!=127)continue;
        if(!xr_value_format_write(sink,bytes+start,n-start))return 0;
        char escaped[6]={'\\','u','0','0',hex[byte>>4],hex[byte&15]};size_t count=6;
        if(byte=='"' || byte=='\\'){escaped[1]=(char)byte;count=2;}
        else if(byte=='\n'){escaped[1]='n';count=2;}
        else if(byte=='\r'){escaped[1]='r';count=2;}
        else if(byte=='\t'){escaped[1]='t';count=2;}
        if(!xr_value_format_write(sink,escaped,count))return 0;
        start=n+1;
    }
    return (!length || xr_value_format_write(sink,bytes+start,length-start)) && xr_value_format_write(sink,"\"",1);
}
#endif
