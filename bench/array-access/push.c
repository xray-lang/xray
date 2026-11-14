#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    size_t cap=4, len=0;
    int64_t *a=malloc(cap*sizeof(*a));
    if (!a) return 2;
    for (int64_t i=0;i<INT64_C(5000000);++i) {
        if (len==cap) {
            if (cap>SIZE_MAX/2/sizeof(*a)) {free(a);return 2;}
            size_t next=cap*2;
            int64_t *replacement=realloc(a,next*sizeof(*a));
            if (!replacement) {free(a);return 2;}
            a=replacement;cap=next;
        }
        a[len++]=i;
    }
    int64_t total=0;
    for(size_t i=0;i<len;++i)total+=a[i];
    printf("%" PRId64 "\n",total);
    free(a);return 0;
}
