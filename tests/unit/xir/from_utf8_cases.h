/* Source and generated C execute the same strict decoder consumer. */
#ifndef FROM_UTF8_CASES_H
#define FROM_UTF8_CASES_H
#ifndef FROM_UTF8_EXPECTED_ONLY
static const char from_utf8_source[] =
    "fn decode(bytes:Slice<u8>) throws(Utf8Error)->string { return try string.fromUtf8(bytes) }\n"
    "var good:Array<u8>=[65,0,195,169]\n"
    "try { print(try decode(good[:])) } catch(e:Utf8Error) { print(\"unexpected\") }\n"
    "var bad:Array<u8>=[255]\n"
    "try { print(try decode(bad[:])) } catch(e:Utf8Error) { print(\"invalid\") }\n";
static const char from_utf8_local_source[]=
    "fn decode(bytes:Slice<u8>) throws(Utf8Error)->string { return try string.fromUtf8(bytes) }\n"
    "fn check() {\n"
    " var good:Array<u8>=[65,0,195,169]\n"
    " try { print(try decode(good[:])) } catch(e:Utf8Error) { print(\"unexpected\") }\n"
    " var bad:Array<u8>=[255]\n"
    " try { print(try decode(bad[:])) } catch(e:Utf8Error) { print(\"invalid\") }\n"
    "}\n"
    "check()\n"
;
static const char from_utf8_owned_source[]=
    "fn text() throws(Utf8Error)->string {\n"
    " var bytes:Array<u8>=[65,0,195,169]\n"
    " const decoded=try string.fromUtf8(bytes[:])\n"
    " bytes[0]=66\n"
    " return decoded\n"
    "}\n"
    "fn empty() throws(Utf8Error)->string { var bytes:Array<u8>=[];return try string.fromUtf8(bytes[:]) }\n"
    "print(try text())\n"
    "print(try empty())\n"
;
static const char from_utf8_invalid_source[]=
    "fn invalid() {\n"
    " var bytes0:Array<u8>=[255]\n"
    " try { print(try string.fromUtf8(bytes0[:])) } catch(e:Utf8Error) { print(e==Utf8Error.InvalidUtf8) }\n"
    " var bytes1:Array<u8>=[192,128]\n"
    " try { print(try string.fromUtf8(bytes1[:])) } catch(e:Utf8Error) { print(e==Utf8Error.InvalidUtf8) }\n"
    " var bytes2:Array<u8>=[237,160,128]\n"
    " try { print(try string.fromUtf8(bytes2[:])) } catch(e:Utf8Error) { print(e==Utf8Error.InvalidUtf8) }\n"
    " var bytes3:Array<u8>=[244,144,128,128]\n"
    " try { print(try string.fromUtf8(bytes3[:])) } catch(e:Utf8Error) { print(e==Utf8Error.InvalidUtf8) }\n"
    " var bytes4:Array<u8>=[226,130]\n"
    " try { print(try string.fromUtf8(bytes4[:])) } catch(e:Utf8Error) { print(e==Utf8Error.InvalidUtf8) }\n"
    "}\n"
    "invalid()\n"
;
#endif
static const char from_utf8_expected[]="A\0\xc3\xa9\ninvalid\n";
static const char from_utf8_owned_expected[]="A\0\xc3\xa9\n\n";
static const char from_utf8_invalid_expected[]="true\ntrue\ntrue\ntrue\ntrue\n";
#endif
