/* Byte-exact output includes an embedded NUL and a newline inside an argument. */
#ifndef NULLABLE_STRING_PRINT_EXPECTED_H
#define NULLABLE_STRING_PRINT_EXPECTED_H
static const char nullable_print_expected[] =
    "null heap-é中😀\0tail\n"
    " null\n"
    "once! 1\n"
    "heap-é中😀\0tail changed changed heap-é中😀\0tail\n"
    "line\nbreak null\n"
    "owned-result\n";
#endif // NULLABLE_STRING_PRINT_EXPECTED_H
