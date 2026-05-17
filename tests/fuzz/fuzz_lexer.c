/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fuzz_lexer.c - Fuzzing harness for the lexer
 *
 * Build with libFuzzer:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -I../../src -I../../src/frontend/lexer \
 *     fuzz_lexer.c ../../src/frontend/lexer/xlex.c \
 *     -o fuzz_lexer
 *
 * Run:
 *   ./fuzz_lexer corpus/ -max_len=4096
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/lexer/xlex.h"

/*
 * libFuzzer entry point
 * Called repeatedly with mutated input data
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Skip empty input */
    if (size == 0) {
        return 0;
    }

    /* Create null-terminated string from fuzzer input */
    char *input = (char *) malloc(size + 1);
    if (!input) {
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';

    /* Initialize scanner */
    Scanner scanner;
    xr_scanner_init(&scanner, input);

    /* Scan all tokens until EOF or error */
    Token token;
    int token_count = 0;
    const int max_tokens = 100000; /* Prevent infinite loops */

    do {
        token = xr_scanner_scan(&scanner);
        token_count++;

        /* Safety: prevent runaway scanning */
        if (token_count > max_tokens) {
            break;
        }
    } while (token.type != TK_EOF && token.type != TK_ERROR);

    free(input);
    return 0;
}

#ifdef FUZZ_STANDALONE
/*
 * Standalone mode for testing without libFuzzer
 * Usage: ./fuzz_lexer_standalone < input.txt
 */
#include <stdio.h>

int main(int argc, char **argv) {
    /* Read from stdin or file */
    FILE *f = stdin;
    if (argc > 1) {
        f = fopen(argv[1], "rb");
        if (!f) {
            perror("fopen");
            return 1;
        }
    }

    /* Read entire input */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    if (!data) {
        fclose(f);
        return 1;
    }
    fread(data, 1, size, f);

    if (f != stdin) {
        fclose(f);
    }

    /* Run fuzzer */
    int result = LLVMFuzzerTestOneInput(data, size);

    free(data);
    printf("Lexer processed %ld bytes successfully\n", size);
    return result;
}
#endif
