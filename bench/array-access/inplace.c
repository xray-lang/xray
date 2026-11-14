#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    const size_t n=4096;
    uint64_t *a=malloc(n*sizeof(*a));
    if (!a) return 2;
    for(size_t i=0;i<n;++i)a[i]=(uint64_t)i;
    for(int r=0;r<20000;++r)
        for(size_t i=0;i<n;++i)a[i]=a[i]*UINT64_C(3)+UINT64_C(1);
    uint64_t total=0;
    for(size_t i=0;i<n;++i)total+=a[i];
    int64_t result=0;memcpy(&result,&total,sizeof(result));
    printf("%" PRId64 "\n",result);
    free(a);return 0;
}
