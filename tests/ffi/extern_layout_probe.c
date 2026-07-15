#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct CScalar {
    uint8_t tag;
    uint32_t count;
} CScalar;

typedef struct CPointer {
    uint8_t *next;
} CPointer;

typedef struct CArray {
    uint16_t samples[3];
} CArray;

typedef struct CHeader {
    uint8_t tag;
    uint32_t count;
    uint8_t *next;
    uint16_t samples[3];
} CHeader;

typedef union CWord {
    uint32_t word;
    uint8_t bytes[4];
} CWord;

typedef struct CEnvelope {
    CHeader header;
    CWord word;
} CEnvelope;

int main(void) {
    printf("%zu\n", sizeof(CScalar));
    printf("%zu\n", sizeof(CPointer));
    printf("%zu\n", sizeof(CArray));
    printf("%zu\n", sizeof(CHeader));
    printf("%zu\n", _Alignof(CHeader));
    printf("%zu\n", offsetof(CHeader, tag));
    printf("%zu\n", offsetof(CHeader, count));
    printf("%zu\n", offsetof(CHeader, next));
    printf("%zu\n", offsetof(CHeader, samples));
    printf("%zu\n", sizeof(CWord));
    printf("%zu\n", _Alignof(CWord));
    printf("%zu\n", offsetof(CWord, word));
    printf("%zu\n", offsetof(CWord, bytes));
    printf("%zu\n", sizeof(CEnvelope));
    printf("%zu\n", _Alignof(CEnvelope));
    printf("%zu\n", offsetof(CEnvelope, header));
    printf("%zu\n", offsetof(CEnvelope, word));
    return 0;
}
