#include <stdio.h>

static double run(long long n) {
    double acc = 0.0;
    double x = 1.0;
    long long i = 0;
    while (i < n) {
        acc = acc + x * 1.000001;
        x = x + 0.000001;
        if (x > 2.0) {
            x = 1.0;
        }
        i = i + 1;
    }
    return acc;
}

int main(void) {
    printf("%.15g\n", run(300000));
    return 0;
}
