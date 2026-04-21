/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fuzz_parser.c - Fuzzing harness for the parser
 *
 * Build with libFuzzer:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -I../../src -I../../src/frontend/lexer -I../../src/frontend/parser \
 *     fuzz_parser.c -L../../build -lxray_core -lm -lpthread \
 *     -o fuzz_parser
 *
 * Run:
 *   ./fuzz_parser corpus/ -max_len=4096
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/lexer/xlex.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "frontend/parser/xast_api.h"

/*
 * libFuzzer entry point
 * Called repeatedly with mutated input data
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Skip empty input */
    if (size == 0) {
        return 0;
    }

    /* Limit input size to prevent memory exhaustion */
    if (size > 65536) {
        return 0;
    }

    /* Create null-terminated string from fuzzer input */
    char *input = (char *)malloc(size + 1);
    if (!input) {
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';

    /* Initialize parser (X=NULL for fuzzing, no isolate needed) */
    Parser parser;
    xr_parser_init(&parser, NULL, input, "<fuzz>", NULL);

    /* Parse the input using recoverable mode - errors are expected */
    AstNode *ast = xr_parse_recoverable(&parser);

    /* Free AST if successfully parsed */
    if (ast) {
        xr_program_destroy(ast);
    }

    free(input);
    return 0;
}

#ifdef FUZZ_STANDALONE
/*
 * Standalone mode for testing without libFuzzer
 * Usage: ./fuzz_parser_standalone < input.xr
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
    printf("Parser processed %ld bytes successfully\n", size);
    return result;
}
#endif
