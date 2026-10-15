/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_derivation.inc.c - Explicit protocol and member-operation intent
 *
 * KEY CONCEPT:
 *   Parsing never grants a capability or selects a compiler-specific protocol.
 */
static bool parse_derive_consume(Parser *parser,XrTokenType type,const char *message) {
    bool matches=xr_parser_check(parser,type);
    xr_parser_consume(parser,type,message);
    return matches && xr_parser_healthy(parser);
}
static bool parse_derive_option(Parser *parser,Token token,uint32_t *operation) {
    if(token.length==8 && !xr_parser_compare_bytes(parser,token.start,"describe",8))
        *operation=XR_DERIVE_FIELD_DESCRIBE;
    else if(token.length==4 && !xr_parser_compare_bytes(parser,token.start,"read",4))
        *operation=XR_DERIVE_FIELD_READ;
    else if(token.length==5 && !xr_parser_compare_bytes(parser,token.start,"write",5))
        *operation=XR_DERIVE_FIELD_WRITE;
    else return false;
    return xr_parser_healthy(parser);
}
static bool parse_derive_fields(Parser *parser,XrDerivationSyntax *syntax,uint32_t operation,int *capacity) {
    if(!parse_derive_consume(parser,TK_LBRACKET,"expected '[' before derive field names"))return false;
    if(!xr_parser_check(parser,TK_RBRACKET))do{
        if(!parse_derive_consume(parser,TK_LITERAL_STRING,"derive fields require literal names"))return false;
        Token token=parser->previous;const char *name=xr_attr_string_arg(parser);
        if(!name || !xr_parser_healthy(parser))return false;
        if(!*name){xr_parser_error_at_previous(parser,"derive field name is empty");return false;}
        XrDeriveFieldSyntax field={name,operation,token.line,token.column};
        XR_PARSE_PUSH(parser,syntax->fields,syntax->field_count,*capacity,field);
        if(!xr_parser_healthy(parser))return false;
    }while(xr_parser_match(parser,TK_COMMA) && !xr_parser_check(parser,TK_RBRACKET));
    return parse_derive_consume(parser,TK_RBRACKET,"expected ']' after derive field names");
}
static XrDerivationSyntax *parse_derivation_intent(Parser *parser) {
    if(!parse_derive_consume(parser,TK_LPAREN,"expected '(' after @derive"))return NULL;
    XrDerivationSyntax *syntax=ast_alloc(parser->compiler_session,sizeof(*syntax));
    if(!syntax || !xr_parser_healthy(parser))return NULL;
    *syntax=(XrDerivationSyntax){0};syntax->protocol=xr_parse_type_annotation(parser);
    if(!syntax->protocol || !xr_parser_healthy(parser))return NULL;
    if(!parse_derive_consume(parser,TK_COMMA,"@derive requires an ordinary generator after its protocol"))return NULL;
    if(!parse_derive_consume(parser,TK_NAME,"expected ordinary derive generator name"))return NULL;
    syntax->generator=xr_parser_token_string(parser,&parser->previous);
    if(!syntax->generator || !xr_parser_healthy(parser))return NULL;
    if(xr_parser_match(parser,TK_DOT)){
        syntax->generator_module=syntax->generator;
        if(!parse_derive_consume(parser,TK_NAME,"expected generator name after module qualifier"))return NULL;
        syntax->generator=xr_parser_token_string(parser,&parser->previous);
        if(!syntax->generator || !xr_parser_healthy(parser))return NULL;
    }
    uint32_t seen=0;int capacity=0;
    while(xr_parser_match(parser,TK_COMMA) && !xr_parser_check(parser,TK_RPAREN)){
        if(!parse_derive_consume(parser,TK_NAME,"expected describe, read, write or construct derive option"))return NULL;
        Token option=parser->previous;uint32_t operation=0;
        bool fields=parse_derive_option(parser,option,&operation);
        if(!xr_parser_healthy(parser))return NULL;
        bool construct=option.length==9 && !xr_parser_compare_bytes(parser,option.start,"construct",9);
        if(!xr_parser_healthy(parser))return NULL;
        if(!fields && !construct){xr_parser_error_at_previous(parser,"unknown derive option");return NULL;}
        uint32_t flag=1u<<(fields?operation:3);
        if(seen&flag){xr_parser_error_at_previous(parser,"duplicate derive option");return NULL;}
        seen|=flag;
        if(!parse_derive_consume(parser,TK_COLON,"expected ':' after derive option"))return NULL;
        if(fields){if(!parse_derive_fields(parser,syntax,operation,&capacity))return NULL;}
        else if(xr_parser_match(parser,TK_TRUE))syntax->construct=true;
        else if(xr_parser_match(parser,TK_FALSE))syntax->construct=false;
        else{xr_parser_error_at_current(parser,"construct requires a literal bool");return NULL;}
    }
    return parse_derive_consume(parser,TK_RPAREN,"expected ')' after explicit derive intent")?syntax:NULL;
}
