/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_input.h - Real lexer observations preserve published state
 */
static void input_observations(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    static const struct {const char *text;XrSourceInputState expected;} rows[]={
        {"",XR_SOURCE_INPUT_COMPLETE},{"42",XR_SOURCE_INPUT_COMPLETE},{"var =",XR_SOURCE_INPUT_COMPLETE},
        {"fn f(){",XR_SOURCE_INPUT_INCOMPLETE},{"fn f(){ return 42 }",XR_SOURCE_INPUT_COMPLETE},
        {"([",XR_SOURCE_INPUT_INCOMPLETE},{"([])",XR_SOURCE_INPUT_COMPLETE},{"#{",XR_SOURCE_INPUT_INCOMPLETE},
        {"#[1,",XR_SOURCE_INPUT_INCOMPLETE},{"#[1]",XR_SOURCE_INPUT_COMPLETE},{"/* {",XR_SOURCE_INPUT_INCOMPLETE},
        {"/* { */ 1",XR_SOURCE_INPUT_COMPLETE},{"// {",XR_SOURCE_INPUT_COMPLETE},
        {"\"中{\"",XR_SOURCE_INPUT_COMPLETE},{"\"unfinished",XR_SOURCE_INPUT_INCOMPLETE},
        {"'a",XR_SOURCE_INPUT_INCOMPLETE},{"'a'",XR_SOURCE_INPUT_COMPLETE},
        {"{ \"}\" }",XR_SOURCE_INPUT_COMPLETE},{"}",XR_SOURCE_INPUT_COMPLETE}};
    for(size_t i=0;i<sizeof(rows)/sizeof(*rows);++i) {
        XrSourceInputState state=XR_SOURCE_INPUT_INCOMPLETE;
        CHECK(xray_source_repl_check_input(owner,rows[i].text,strlen(rows[i].text),&state)==XR_SOURCE_OK && state==rows[i].expected);
    }
    char span[3]={'4','2','{'};XrSourceInputState state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,span,2,&state)==XR_SOURCE_OK && state==XR_SOURCE_INPUT_COMPLETE);
    char invalid[3]={'4',0,'2'};state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,invalid,3,&state)==XR_SOURCE_INVALID && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(xray_source_repl_check_input(owner,NULL,1,&state)==XR_SOURCE_BAD_ARGUMENT && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(xray_source_repl_check_input(owner,NULL,0,&state)==XR_SOURCE_OK && state==XR_SOURCE_INPUT_COMPLETE);
    CHECK(begin(owner,"41\n",NULL)==XR_SOURCE_OK);state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,"42",2,&state)==XR_SOURCE_BUSY && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
    integer(owner,"it",41);state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,"42",2,&state)==XR_SOURCE_OK && state==XR_SOURCE_INPUT_COMPLETE);integer(owner,"it",41);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    puts("Input observation: real delimiters/literals/comments, exact nonterminated span, pending exclusion and unchanged it PASS");
}
