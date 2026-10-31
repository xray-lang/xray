/* The generated C entry consumes the same fixed independent oracle as the VM. */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"whitespace native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "rune_whitespace_cases.h"
XR_FUNC XrXirRunStatus rune_whitespace_f0(XrXirRunContext *,const XrXirValue *,uint32_t,XrXirValue *);
static XrXirRunStatus whitespace_native(void *owner,XrXirRunContext *run,const XrXirValue *input,XrXirValue *output) {
    (void)owner;return rune_whitespace_f0(run,input,1,output);
}
int main(void) {whitespace_cases(whitespace_native,NULL);return 0;}
