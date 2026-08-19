/* What a BigInt literal's text is allowed to say.
 *
 * The digits of an arbitrary-precision literal travel as text from the front
 * end all the way to the runtime, because no fixed-width slot can hold them.
 * Every layer along the way needs the same answer to "which base is this, and
 * where do its digits start" -- the semantic plan to state the constant, the
 * runtime to build the value. Written twice, the two answers drift; written
 * here, they cannot.
 */
#ifndef XR_BIGINT_LITERAL_CORE_H
#define XR_BIGINT_LITERAL_CORE_H

#include <stddef.h>

typedef struct {
    const char *digits; /* First digit character, past sign and base prefix. */
    int base;           /* 2, 8, 10 or 16. */
    int negative;       /* 1 when a leading '-' was consumed. */
} XrBigIntLiteralCore;

/* Split a literal into sign, base and digit run. The text is not validated
 * here -- digits may still be empty or out of range for the base, which is
 * what xr_bigint_literal_is_wellformed_core answers. */
static inline XrBigIntLiteralCore xr_bigint_literal_split_core(const char *text) {
    XrBigIntLiteralCore out = {NULL, 10, 0};
    if (!text)
        return out;
    if (*text == '-') {
        out.negative = 1;
        text++;
    } else if (*text == '+') {
        text++;
    }
    if (text[0] == '0' && text[1] != '\0') {
        if (text[1] == 'x' || text[1] == 'X') {
            out.base = 16;
            text += 2;
        } else if (text[1] == 'b' || text[1] == 'B') {
            out.base = 2;
            text += 2;
        } else if (text[1] == 'o' || text[1] == 'O') {
            out.base = 8;
            text += 2;
        }
    }
    out.digits = text;
    return out;
}

static inline int xr_bigint_literal_digit_value_core(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* True when the text is a literal the runtime can turn into a value: a digit
 * run that is non-empty and entirely in range for the base it declared. */
static inline int xr_bigint_literal_is_wellformed_core(const char *text) {
    XrBigIntLiteralCore split = xr_bigint_literal_split_core(text);
    if (!split.digits || !*split.digits)
        return 0;
    for (const char *c = split.digits; *c; c++) {
        int value = xr_bigint_literal_digit_value_core(*c);
        if (value < 0 || value >= split.base)
            return 0;
    }
    return 1;
}

#endif /* XR_BIGINT_LITERAL_CORE_H */
