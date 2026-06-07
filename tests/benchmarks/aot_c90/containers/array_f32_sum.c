#include <stdio.h>

#define ARRAY_F32_SUM_N 200000

static double run(long long n) {
    float values[ARRAY_F32_SUM_N];
    long long i = 0;
    double x = 1.0;
    while (i < n) {
        values[i] = (float) x;
        x = x + 0.25;
        if (x > 17.0) {
            x = 1.0;
        }
        i = i + 1;
    }

    double sum = 0.0;
    i = 0;
    while (i < n) {
        sum = sum + (double) values[i];
        i = i + 1;
    }
    return sum;
}

int main(void) {
    printf("%.15g\n", run(ARRAY_F32_SUM_N));
    return 0;
}
