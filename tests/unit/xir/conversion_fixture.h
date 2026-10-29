/* Complete Source conversion consumer shared by the installed SDK and CLI. */
#ifndef XR_TEST_CONVERSION_FIXTURE_H
#define XR_TEST_CONVERSION_FIXTURE_H
static const char conversion_source[]=
    "enum Failure{Bad{message:string}}\r\n"
    "fn fail(value:Error?) throws(Error)->i64{throw value!}\r\n"
    "fn apply(callback:fn(i64)->i64)->i64{return callback(41)}\r\n"
    "fn answer()->i64{\r\n"
    " const narrow:i8=7;const wide:i64=narrow;const lifted:i64?=wide\r\n"
    " const pair:(i64,i64?)=(narrow,lifted);const branch:i64=true?narrow:wide\r\n"
    " const callback:fn(i64)->i64=fn(value:i64)->i64{return value+1}\r\n"
    " const optional:(fn(i64)->i64)?=callback\r\n"
    " const erased:Error=Failure.Bad{message:\"owned\"};const error:Error?=erased\r\n"
    " try{return try fail(error)}catch(e:Failure){return pair.0+branch+optional!(27)+(lifted!-7)}catch(e){return -1}\r\n"
    "}\r\n"
    "const inferred=fn(value:i64){return value+1}\r\n"
    "print(answer(),apply(fn(value:i64)->i64{return value+1}))\r\nprint(\"cv\\0\xe4\xb8\xad\")\r\n";
static const char conversion_expected[]="42 42\ncv\0\xe4\xb8\xad\n";
#endif // XR_TEST_CONVERSION_FIXTURE_H
