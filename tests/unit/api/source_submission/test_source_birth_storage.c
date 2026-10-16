/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_birth_storage.c - Independent storage packets and actual runtime refusal
 */
#define main existing_submission_cases
#include "test_source_submission.c"
#undef main
#include "source_birth_storage_contract.h"
int main(void) {
    birth_storage_contract();return 0;
}
