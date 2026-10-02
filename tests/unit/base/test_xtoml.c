/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtoml.c - TOML duplicate assignment and insertion ownership checks
 */
#include "test_xtoml_helpers.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned explicit_lengths(void) {
    const char *source = "\"a\\u0000b\"='first'\na='plain'\n\"a\\u0000c\"=\"x\\u0000y\"\n";
    XrTomlValue *root = test_toml_parse(source, strlen(source));
    if (!root) return 1;
    unsigned failures = 0;
    const char *plain = test_toml_get_string(root, "a");
    if (root->as.table.count != 3 || !plain || strcmp(plain, "plain")) ++failures;
    if (root->as.table.count == 3) {
        const XrTomlMember *first = &root->as.table.members[0], *last = &root->as.table.members[2];
        if (first->key_length != 3 || memcmp(first->key, "a\0b", 3)) ++failures;
        if (last->key_length != 3 || memcmp(last->key, "a\0c", 3)) ++failures;
        if (last->value->string_length != 3 || memcmp(last->value->as.string, "x\0y", 3)) ++failures;
    }
    xtoml_owned_free(root);
    const char *duplicate = "\"a\\u0000b\"=1\n\"a\\u0000b\"=2\n";
    root = test_toml_parse(duplicate, strlen(duplicate));
    if (root) ++failures;
    xtoml_owned_free(root);
    return failures;
}

static unsigned array_table_identity(void) {
    const char *source = "[[a]]\nx=1\n[a.b]\ny=2\n[[a]]\nx=3\n[a.b]\ny=4\n";
    XrTomlValue *root = test_toml_parse(source, strlen(source));
    if (!root) return 1;
    XrTomlValue *items = test_toml_get_array(root, "a");
    unsigned failures = 0;
    if (xtoml_array_len(items) != 2) ++failures;
    for (int i = 0; i < 2; ++i) {
        XrTomlValue *item = xtoml_array_get(items, i);
        if (test_toml_get_int_or(item, "x", -1) != 2 * i + 1) ++failures;
        if (test_toml_get_int_or(test_toml_get_table(item, "b"), "y", -1) != 2 * i + 2) ++failures;
    }
    xtoml_owned_free(root);
    return failures;
}

static unsigned escaped_values(void) {
    const char *source =
        "continued=\"\"\"first\\ \t\r\n \t\n second\"\"\"\n"
        "one=\"\"\"x\"\"\"\"\n"
        "two='''x'''''\n"
        "escaped=\"\\b\\t\\n\\f\\r\\\"\\\\\\u0041\\U0001F600\"\n";
    XrTomlValue *root = test_toml_parse(source, strlen(source));
    if (!root) return 1;
    const char *continued = test_toml_get_string(root, "continued");
    const char *escaped = test_toml_get_string(root, "escaped");
    unsigned failures = 0;
    if (!continued || strcmp(continued, "firstsecond")) ++failures;
    const char *one = test_toml_get_string(root, "one"), *two = test_toml_get_string(root, "two");
    if (!one || strcmp(one, "x\"")) ++failures;
    if (!two || strcmp(two, "x''")) ++failures;
    if (!escaped || strcmp(escaped, "\b\t\n\f\r\"\\A\xF0\x9F\x98\x80")) ++failures;
    xtoml_owned_free(root);
    return failures;
}

static unsigned raw_text_admission(void) {
    static const char *const rejected[] = {
        "a='\xC0\x80'\n", "a='\x80'\n", "a='\xE2\x82'\n",
        "a='\xED\xA0\x80'\n", "a='\xF4\x90\x80\x80'\n",
        "a=\"\x01\"\n", "a='\x7F'\n", "#\x08\n", "a=1\r",
        "a=\"\"\"x\rtext\"\"\"\n", "#\xFF\n"
    };
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        XrTomlValue *root = test_toml_parse(rejected[i], strlen(rejected[i]));
        if (root) ++failures;
        xtoml_owned_free(root);
    }
    const char nul[] = "a='x\0y'\n";
    XrTomlValue *root = test_toml_parse(nul, sizeof(nul) - 1);
    if (root) ++failures;
    xtoml_owned_free(root);
    const char valid[] = "a='\xF0\x9F\x98\x80'\r\n#\xE4\xB8\xAD\n";
    root = test_toml_parse(valid, sizeof(valid) - 1);
    const char *value = test_toml_get_string(root, "a");
    if (!value || strcmp(value, "\xF0\x9F\x98\x80")) ++failures;
    xtoml_owned_free(root);
    return failures;
}

static unsigned numeric_values(void) {
    const char *source = "min=-9223372036854775808\nmax=9223372036854775807\n"
        "hex=0x7fff_ffff_ffff_ffff\noct=0o755\nbin=0b1010_0010\nf=1_000.2_5e+0_2\nz=-0.0\n";
    XrTomlValue *root = test_toml_parse(source, strlen(source));
    if (!root) return 1;
    unsigned failures = 0;
    if (test_toml_get_int(root, "min") != INT64_MIN || test_toml_get_int(root, "max") != INT64_MAX) ++failures;
    if (test_toml_get_int(root, "hex") != INT64_MAX || test_toml_get_int(root, "oct") != 493 ||
        test_toml_get_int(root, "bin") != 162) ++failures;
    if (test_toml_get_float(root, "f") != 100025.0 || !signbit(test_toml_get_float(root, "z"))) ++failures;
    xtoml_owned_free(root);
    const char *const overflow[] = {
        "a=9223372036854775808\n", "a=-9223372036854775809\n", "a=0x8000000000000000\n"
    };
    for (size_t i = 0; i < sizeof(overflow) / sizeof(overflow[0]); ++i) {
        root = test_toml_parse(overflow[i], strlen(overflow[i]));
        if (root) ++failures;
        xtoml_owned_free(root);
    }
    return failures;
}

static unsigned datetime_values(void) {
    const char *const values[] = {"2024-02-29", "07:32:00.123456789",
        "1979-05-27 07:32:00.123456789+01:30", "1979-05-27t07:32:00z"};
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        char source[96];
        int length = snprintf(source, sizeof(source), "a=%s\n", values[i]);
        if (length < 0 || (size_t) length >= sizeof(source)) return failures + 1;
        XrTomlValue *root = test_toml_parse(source, (size_t) length);
        XrTomlValue *value = test_toml_get(root, "a");
        if (!value || value->type != XR_TOML_DATETIME ||
            value->string_length != strlen(values[i]) || strcmp(value->as.string, values[i])) ++failures;
        xtoml_owned_free(root);
    }
    return failures;
}

int main(void) {
    static const char *const rejected[] = {
        "key=1\nkey=2\n",
        "key=1\n\"key\"=2\n",
        "a.b=1\na.b=2\n",
        "a=1\na.b=2\n",
        "a.b=1\na=2\n",
        "a={b=1,b=2}\n",
        "a={b=1,b.c=2}\n",
        "[native.symbol.contract]\nsuspend='never'\nsuspend='may'\n",
        "[a]\nx=1\n[a]\ny=2\n",
        "a.b=1\n[a]\nc=2\n",
        "a={b=1}\na.c=2\n",
        "a={}\n[a.b]\nx=1\n",
        "a=[]\n[[a]]\nx=1\n",
        "a=[{b=1}]\n[a.c]\nx=1\n",
        "[[a]]\nx=1\n[a]\ny=2\n",
        "[a.b]\nx=1\n[a]\nb.y=2\n",
        "a={b={c=1},b.d=2}\n",
        "a={b=1}\n[[a.c]]\nx=1\n",
        "version=1 junk\n",
        "flag=true false\n",
        "[declarations] junk\nversion=1\n",
        "[[items]] junk\nx=1\n",
        "name=\"bad\\q\"\n",
        "name=\"bad\\\ncontinued\"\n",
        "name=\"\"\"bad\\  text\"\"\"\n",
        "\"\"\"key\"\"\"=1\n",
        "'''key'''=1\n",
        "a=\"\"\"x\"\"\"\"\"\"\n",
        "a='''x''''''\n",
        "version=1+2\n", "version=01\n", "version=1_\n",
        "a=+\n", "a=-\n", "a=1__2\n", "a=0_1\n",
        "a=0x\n", "a=0x_FF\n", "a=-0x1\n", "a=+0o1\n", "a=0X1\n",
        "a=1.\n", "a=1.e2\n", "a=1e\n", "a=1e+\n",
        "a=1.2.3\n", "a=1e2e3\n", "a=1._2\n", "a=1e_2\n",
        "a=01.0\n", "a=0b_1\n", "a=0o_1\n", "a=0x1_\n",
        "a=2023-02-29\n", "a=1900-02-29\n", "a=2024-04-31\n",
        "a=2024-00-01\n", "a=2024-13-01\n", "a=2024-01-00\n",
        "a=2024-01-01T24:00:00Z\n", "a=2024-01-01T00:60:00Z\n",
        "a=2024-01-01T00:00:61Z\n", "a=2024-01-01T00:00:00+24:00\n",
        "a=2024-01-01T00:00:00+00:60\n", "a=2024-01-01T00:00:00.\n",
        "a=2024-01-01T00:00\n", "a=2024-01-01Z\n",
        "a=07:32:00Z\n", "a=07:32:00+01:00\n"
    };
    static const char *const accepted[] = {
        "a.b=1\na.c=2\n",
        "[[items]]\nx=1\n[[items]]\nx=2\n",
        "a={b=1,c=2}\n",
        "[a.b]\nx=1\n[a]\ny=2\n",
        "a.b.c=1\na.b.d=2\n",
        "[a.b.c]\nx=1\n[a]\nb.z=2\n",
        "a.b.c=1\n[a.b.d]\nx=2\n",
        "[[a]]\nx=1\n[a.b]\ny=2\n[[a]]\nx=3\n[a.b]\ny=4\n",
        "a={b.c=1,b.d=2}\n",
        "a=1 # allowed comment\n[b] # header comment\nx=true\n",
        "a=\"\"\"first\\ \t\n second\"\"\"\n",
        "a=\"\\b\\t\\n\\f\\r\\\"\\\\\\u0041\\U0001F600\"\n",
        "a=\"\"\"x\"\"\"\"\n",
        "a=\"\"\"x\"\"\"\"\"\n",
        "a='''x''''\n",
        "a='''x'''''\n",
        "a=[0,+0,-0,1_000,0xDEAD_BEEF,0o755,0b1010_0010]\n",
        "a=[1.0,-0.0,+1.25,1e2,1E-2,1_000.2_5e+0_2,inf,-inf,+nan]\n",
        "a=[2024-02-29,2000-02-29,1979-05-27T07:32:00Z]\n",
        "a=[1979-05-27 07:32:00.123456789+01:30,1979-05-27t07:32:00z]\n",
        "a=[07:32:00,00:32:00.999999,1979-05-27T07:32:00]\n"
    };
    unsigned failures = explicit_lengths() + array_table_identity() + escaped_values() + raw_text_admission();
    failures += numeric_values();
    failures += datetime_values();
    for (size_t i = 0; i < sizeof(rejected)/sizeof(rejected[0]); ++i) {
        XrTomlValue *value = test_toml_parse(rejected[i], strlen(rejected[i]));
        if (value) {
            fprintf(stderr, "accepted conflicting assignment %zu\n", i);
            ++failures;
        }
        xtoml_owned_free(value);
    }
    for (size_t i = 0; i < sizeof(accepted)/sizeof(accepted[0]); ++i) {
        XrTomlValue *value = test_toml_parse(accepted[i], strlen(accepted[i]));
        if (!value) {
            fprintf(stderr, "rejected valid document %zu\n", i);
            ++failures;
        }
        xtoml_owned_free(value);
    }
    printf("TOML assignment checks: %u failures\n", failures);
    return failures ? 1 : 0;
}
