/* Exercise the exact command's final stdout failure after successful rename. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fixture_flush(FILE *stream) {
    int result=fflush(stream);
    const char *mode=getenv("XR_FMT_TEST_MODE");
    if(stream==stdout && mode && !strcmp(mode,"output-failure"))return EOF;
    return result;
}
#define fflush fixture_flush
#include "app/cli/xcmd_fmt.c"
