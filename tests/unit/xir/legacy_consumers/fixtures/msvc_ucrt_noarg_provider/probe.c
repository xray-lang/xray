/* Check the selected CRT through typed void and no-argument function pointers. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(_MSC_VER) || !defined(_WIN64)
#error This probe requires the selected MSVC x64 UCRT provider
#endif

_Static_assert(sizeof(unsigned int) * CHAR_BIT == 32, "UCRT state must be 32 bits");
_Static_assert(sizeof(int) == 4, "Selected rand result must be i32");
_Static_assert(RAND_MAX == 32767, "Selected UCRT output mask changed");

extern double xrayffi_unused_float(double value);

int main(void)
{
    void (__cdecl *seed_value)(unsigned int) = srand;
    int (__cdecl *next_value)(void) = rand;
    const unsigned int seeds[] = {0u, 7u, UINT_MAX};
    for (unsigned int i = 0; i < 3u; ++i) {
        seed_value(seeds[i]);
        for (unsigned int j = 0; j < 8u; ++j) {
            printf("%u:%u:%d\n", seeds[i], j + 1u, next_value());
        }
    }
    return 0;
}
