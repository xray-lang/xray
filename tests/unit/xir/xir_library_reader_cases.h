/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_reader_cases.h - Cumulative decoding budgets
 *
 * KEY CONCEPT:
 *   Failed decoding consumes real work without publishing owned prefixes.
 */
#ifndef XIR_LIBRARY_READER_CASES_H
#define XIR_LIBRARY_READER_CASES_H
static void reader_remaining_cases(const uint8_t *bytes,size_t length) {
    CHECK(length>=80);
    XrXirBudget remaining=xr_xir_default_budget(),initial=remaining;
    XrXirArtifact *first=NULL,*second=NULL;
    size_t live=runtime_live,physical=runtime_bytes;
    CHECK(xr_xir_checked_read_remaining(bytes,length,&remaining,&first,NULL)==XR_XIR_OK);
    uint64_t cost=initial.work-remaining.work;
    CHECK(cost>=2*length-32&&remaining.scratch_bytes==initial.scratch_bytes);
    uint64_t after_first=remaining.work;
    CHECK(xr_xir_checked_read_remaining(bytes,length,&remaining,&second,NULL)==XR_XIR_OK);
    CHECK(after_first-remaining.work==cost&&remaining.scratch_bytes==initial.scratch_bytes);
    xr_xir_artifact_free(first);xr_xir_artifact_free(second);
    CHECK(runtime_live==live&&runtime_bytes==physical);
    /* Force the second read to hit the documented capacity preflight after the
     * first consumed its actual work. No returned artifact and no reset. */
    remaining=initial;remaining.work=cost+4*length-1;first=NULL;second=NULL;
    CHECK(xr_xir_checked_read_remaining(bytes,length,&remaining,&first,NULL)==XR_XIR_OK);
    CHECK(remaining.work==4*length-1);after_first=remaining.work;
    CHECK(xr_xir_checked_read_remaining(bytes,length,&remaining,&second,NULL)==XR_XIR_BUDGET&&!second);
    CHECK(remaining.work==after_first);xr_xir_artifact_free(first);
    CHECK(runtime_live==live&&runtime_bytes==physical);
    /* New private wire: kind, function_count, declarations_present, first name.
     * A valid digest forces actual decoding and a partial owned function array. */
    uint8_t *bad=malloc(length);CHECK(bad);memcpy(bad,bytes,length);memset(bad+76,255,4);
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bad,32);
    xr_sha256_update(&hash,bad+64,length-64);xr_sha256_final(&hash,bad+32);
    remaining=initial;first=NULL;
    CHECK(xr_xir_checked_read_remaining(bad,length,&remaining,&first,NULL)==XR_XIR_BAD_STRUCTURE&&!first);
    CHECK(initial.work-remaining.work==length+48);
    CHECK(remaining.metadata_bytes==initial.metadata_bytes&&remaining.scratch_bytes==initial.scratch_bytes);
    CHECK(runtime_live==live&&runtime_bytes==physical);free(bad);
}

#endif // XIR_LIBRARY_READER_CASES_H
