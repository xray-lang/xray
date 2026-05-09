/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xunicode.c - Unicode character classification implementation
 *
 * Unicode codepoint range data from Unicode 15.0 standard.
 * Simplified ranges covering most common characters.
 */

#include "xunicode.h"
#include "xchecks.h"
#include <string.h>
#include <ctype.h>

/* ========== Unicode Range Tables ========== */

// Letter (simplified ranges covering major languages)
static const XrUnicodeRange ranges_L[] = {
    {0x0041, 0x005A},  // A-Z
    {0x0061, 0x007A},  // a-z
    {0x00C0, 0x00D6},  // À-Ö
    {0x00D8, 0x00F6},  // Ø-ö
    {0x00F8, 0x00FF},  // ø-ÿ
    {0x0100, 0x017F},  // Latin Extended-A
    {0x0180, 0x024F},  // Latin Extended-B
    {0x0370, 0x03FF},  // Greek and Coptic
    {0x0400, 0x04FF},  // Cyrillic
    {0x0590, 0x05FF},  // Hebrew
    {0x0600, 0x06FF},  // Arabic
    {0x3040, 0x309F},  // Hiragana
    {0x30A0, 0x30FF},  // Katakana
    {0x4E00, 0x9FFF},  // CJK Unified Ideographs
    {0xAC00, 0xD7AF},  // Hangul Syllables
};
#define RANGES_L_COUNT (sizeof(ranges_L) / sizeof(ranges_L[0]))

// Uppercase Letter
static const XrUnicodeRange ranges_Lu[] = {
    {0x0041, 0x005A},  // A-Z
    {0x00C0, 0x00D6},  // À-Ö
    {0x00D8, 0x00DE},  // Ø-Þ
    {0x0100, 0x0100},  // Ā
    {0x0391, 0x03A9},  // Greek capitals Α-Ω
    {0x0410, 0x042F},  // Cyrillic capitals А-Я
};
#define RANGES_Lu_COUNT (sizeof(ranges_Lu) / sizeof(ranges_Lu[0]))

// Lowercase Letter
static const XrUnicodeRange ranges_Ll[] = {
    {0x0061, 0x007A},  // a-z
    {0x00DF, 0x00F6},  // ß-ö
    {0x00F8, 0x00FF},  // ø-ÿ
    {0x03B1, 0x03C9},  // Greek lowercase α-ω
    {0x0430, 0x044F},  // Cyrillic lowercase а-я
};
#define RANGES_Ll_COUNT (sizeof(ranges_Ll) / sizeof(ranges_Ll[0]))

// Number
static const XrUnicodeRange ranges_N[] = {
    {0x0030, 0x0039},  // 0-9
    {0x0660, 0x0669},  // Arabic-Indic digits
    {0x06F0, 0x06F9},  // Extended Arabic-Indic
    {0x0966, 0x096F},  // Devanagari digits
    {0x09E6, 0x09EF},  // Bengali digits
    {0x0A66, 0x0A6F},  // Gurmukhi digits
    {0xFF10, 0xFF19},  // Fullwidth digits
};
#define RANGES_N_COUNT (sizeof(ranges_N) / sizeof(ranges_N[0]))

// Decimal Number (Nd)
static const XrUnicodeRange ranges_Nd[] = {
    {0x0030, 0x0039},  // 0-9
    {0x0660, 0x0669},  // Arabic-Indic digits
    {0x06F0, 0x06F9},  // Extended Arabic-Indic
    {0xFF10, 0xFF19},  // Fullwidth digits
};
#define RANGES_Nd_COUNT (sizeof(ranges_Nd) / sizeof(ranges_Nd[0]))

// Punctuation
static const XrUnicodeRange ranges_P[] = {
    {0x0021, 0x002F},  // !"#$%&'()*+,-./
    {0x003A, 0x0040},  // :;<=>?@
    {0x005B, 0x0060},  // [\]^_`
    {0x007B, 0x007E},  // {|}~
    {0x00A1, 0x00BF},  // ¡-¿
    {0x2000, 0x206F},  // General Punctuation
    {0x3000, 0x303F},  // CJK Symbols and Punctuation
    {0xFF01, 0xFF0F},  // Fullwidth punctuation
};
#define RANGES_P_COUNT (sizeof(ranges_P) / sizeof(ranges_P[0]))

// Symbol
static const XrUnicodeRange ranges_S[] = {
    {0x0024, 0x0024},    // $
    {0x002B, 0x002B},    // +
    {0x003C, 0x003E},    // <=>
    {0x005E, 0x005E},    // ^
    {0x0060, 0x0060},    // `
    {0x007C, 0x007C},    // |
    {0x007E, 0x007E},    // ~
    {0x00A2, 0x00A9},    // Currency symbols
    {0x2190, 0x21FF},    // Arrows
    {0x2200, 0x22FF},    // Mathematical Operators
    {0x2300, 0x23FF},    // Miscellaneous Technical
    {0x25A0, 0x25FF},    // Geometric Shapes
    {0x2600, 0x26FF},    // Miscellaneous Symbols
    {0x2700, 0x27BF},    // Dingbats
    {0x1F300, 0x1F9FF},  // Emoji
};
#define RANGES_S_COUNT (sizeof(ranges_S) / sizeof(ranges_S[0]))

// Separator (whitespace)
static const XrUnicodeRange ranges_Z[] = {
    {0x0020, 0x0020},  // Space
    {0x00A0, 0x00A0},  // No-Break Space
    {0x1680, 0x1680},  // Ogham Space Mark
    {0x2000, 0x200A},  // Various spaces
    {0x2028, 0x2029},  // Line/Paragraph Separator
    {0x202F, 0x202F},  // Narrow No-Break Space
    {0x205F, 0x205F},  // Medium Mathematical Space
    {0x3000, 0x3000},  // Ideographic Space
};
#define RANGES_Z_COUNT (sizeof(ranges_Z) / sizeof(ranges_Z[0]))

// Other (control chars)
static const XrUnicodeRange ranges_C[] = {
    {0x0000, 0x001F},  // C0 controls
    {0x007F, 0x009F},  // DEL and C1 controls
    {0xD800, 0xDFFF},  // Surrogates
    {0xFFF0, 0xFFFF},  // Specials
};
#define RANGES_C_COUNT (sizeof(ranges_C) / sizeof(ranges_C[0]))

// Han (CJK)
static const XrUnicodeRange ranges_Han[] = {
    {0x3400, 0x4DBF},    // CJK Extension A
    {0x4E00, 0x9FFF},    // CJK Unified Ideographs
    {0xF900, 0xFAFF},    // CJK Compatibility
    {0x20000, 0x2A6DF},  // Extension B
    {0x2A700, 0x2B73F},  // Extension C
    {0x2B740, 0x2B81F},  // Extension D
    {0x2B820, 0x2CEAF},  // Extension E
    {0x2CEB0, 0x2EBEF},  // Extension F
    {0x30000, 0x3134F},  // Extension G
};
#define RANGES_Han_COUNT (sizeof(ranges_Han) / sizeof(ranges_Han[0]))

// Hiragana
static const XrUnicodeRange ranges_Hiragana[] = {
    {0x3040, 0x309F},
    {0x1B001, 0x1B11F},
};
#define RANGES_Hiragana_COUNT (sizeof(ranges_Hiragana) / sizeof(ranges_Hiragana[0]))

// Katakana
static const XrUnicodeRange ranges_Katakana[] = {
    {0x30A0, 0x30FF},
    {0x31F0, 0x31FF},  // Phonetic Extensions
    {0xFF65, 0xFF9F},  // Halfwidth
};
#define RANGES_Katakana_COUNT (sizeof(ranges_Katakana) / sizeof(ranges_Katakana[0]))

// Latin
static const XrUnicodeRange ranges_Latin[] = {
    {0x0041, 0x005A},                                                        // A-Z
    {0x0061, 0x007A},                                                        // a-z
    {0x00C0, 0x00D6}, {0x00D8, 0x00F6}, {0x00F8, 0x00FF}, {0x0100, 0x017F},  // Extended-A
    {0x0180, 0x024F},                                                        // Extended-B
    {0x1E00, 0x1EFF},                                                        // Extended Additional
    {0x2C60, 0x2C7F},                                                        // Extended-C
    {0xA720, 0xA7FF},                                                        // Extended-D
    {0xFF21, 0xFF3A},                                                        // Fullwidth uppercase
    {0xFF41, 0xFF5A},                                                        // Fullwidth lowercase
};
#define RANGES_Latin_COUNT (sizeof(ranges_Latin) / sizeof(ranges_Latin[0]))

// Greek
static const XrUnicodeRange ranges_Greek[] = {
    {0x0370, 0x03FF},
    {0x1F00, 0x1FFF},  // Extended
};
#define RANGES_Greek_COUNT (sizeof(ranges_Greek) / sizeof(ranges_Greek[0]))

// Cyrillic
static const XrUnicodeRange ranges_Cyrillic[] = {
    {0x0400, 0x04FF},
    {0x0500, 0x052F},  // Supplement
    {0x2DE0, 0x2DFF},  // Extended-A
    {0xA640, 0xA69F},  // Extended-B
};
#define RANGES_Cyrillic_COUNT (sizeof(ranges_Cyrillic) / sizeof(ranges_Cyrillic[0]))

// Arabic
static const XrUnicodeRange ranges_Arabic[] = {
    {0x0600, 0x06FF}, {0x0750, 0x077F},  // Supplement
    {0x08A0, 0x08FF},                    // Extended-A
    {0xFB50, 0xFDFF},                    // Presentation Forms-A
    {0xFE70, 0xFEFF},                    // Presentation Forms-B
};
#define RANGES_Arabic_COUNT (sizeof(ranges_Arabic) / sizeof(ranges_Arabic[0]))

// Hebrew
static const XrUnicodeRange ranges_Hebrew[] = {
    {0x0590, 0x05FF},
    {0xFB1D, 0xFB4F},  // Presentation Forms
};
#define RANGES_Hebrew_COUNT (sizeof(ranges_Hebrew) / sizeof(ranges_Hebrew[0]))

// ASCII
static const XrUnicodeRange ranges_ASCII[] = {
    {0x0000, 0x007F},
};
#define RANGES_ASCII_COUNT 1

// Any
static const XrUnicodeRange ranges_Any[] = {
    {0x0000, 0x10FFFF},
};
#define RANGES_Any_COUNT 1

/* ========== Property Name Table ========== */

typedef struct {
    const char *name;
    XrUnicodeProperty prop;
} PropertyEntry;

static const PropertyEntry property_table[] = {
    // General Category
    {"L", XR_UP_L},
    {"Letter", XR_UP_L},
    {"Lu", XR_UP_Lu},
    {"Uppercase_Letter", XR_UP_Lu},
    {"Ll", XR_UP_Ll},
    {"Lowercase_Letter", XR_UP_Ll},
    {"N", XR_UP_N},
    {"Number", XR_UP_N},
    {"Nd", XR_UP_Nd},
    {"Decimal_Number", XR_UP_Nd},
    {"P", XR_UP_P},
    {"Punctuation", XR_UP_P},
    {"S", XR_UP_S},
    {"Symbol", XR_UP_S},
    {"Z", XR_UP_Z},
    {"Separator", XR_UP_Z},
    {"C", XR_UP_C},
    {"Other", XR_UP_C},

    // Scripts
    {"Han", XR_UP_Han},
    {"Hiragana", XR_UP_Hiragana},
    {"Katakana", XR_UP_Katakana},
    {"Latin", XR_UP_Latin},
    {"Greek", XR_UP_Greek},
    {"Cyrillic", XR_UP_Cyrillic},
    {"Arabic", XR_UP_Arabic},
    {"Hebrew", XR_UP_Hebrew},

    // Special
    {"ASCII", XR_UP_ASCII},
    {"Any", XR_UP_Any},

    {NULL, XR_UP_INVALID}};

/* ========== API Implementation ========== */

XrUnicodeProperty xr_unicode_property_lookup(const char *name, int len) {
    XR_DCHECK(len >= 0, "unicode_property_lookup: negative len");
    if (!name || len <= 0)
        return XR_UP_INVALID;

    for (const PropertyEntry *e = property_table; e->name != NULL; e++) {
        if ((int) strlen(e->name) == len && strncmp(e->name, name, len) == 0) {
            return e->prop;
        }
    }

    // Case-insensitive fallback
    for (const PropertyEntry *e = property_table; e->name != NULL; e++) {
        if ((int) strlen(e->name) == len) {
            bool match = true;
            for (int i = 0; i < len; i++) {
                if (tolower((unsigned char) name[i]) != tolower((unsigned char) e->name[i])) {
                    match = false;
                    break;
                }
            }
            if (match)
                return e->prop;
        }
    }

    return XR_UP_INVALID;
}

bool xr_unicode_property_ranges(XrUnicodeProperty prop, const XrUnicodeRange **out_ranges,
                                int *out_count) {
    if (!out_ranges || !out_count)
        return false;

    switch (prop) {
        case XR_UP_L:
            *out_ranges = ranges_L;
            *out_count = RANGES_L_COUNT;
            return true;
        case XR_UP_Lu:
            *out_ranges = ranges_Lu;
            *out_count = RANGES_Lu_COUNT;
            return true;
        case XR_UP_Ll:
            *out_ranges = ranges_Ll;
            *out_count = RANGES_Ll_COUNT;
            return true;
        case XR_UP_N:
            *out_ranges = ranges_N;
            *out_count = RANGES_N_COUNT;
            return true;
        case XR_UP_Nd:
            *out_ranges = ranges_Nd;
            *out_count = RANGES_Nd_COUNT;
            return true;
        case XR_UP_P:
            *out_ranges = ranges_P;
            *out_count = RANGES_P_COUNT;
            return true;
        case XR_UP_S:
            *out_ranges = ranges_S;
            *out_count = RANGES_S_COUNT;
            return true;
        case XR_UP_Z:
            *out_ranges = ranges_Z;
            *out_count = RANGES_Z_COUNT;
            return true;
        case XR_UP_C:
            *out_ranges = ranges_C;
            *out_count = RANGES_C_COUNT;
            return true;
        case XR_UP_Han:
            *out_ranges = ranges_Han;
            *out_count = RANGES_Han_COUNT;
            return true;
        case XR_UP_Hiragana:
            *out_ranges = ranges_Hiragana;
            *out_count = RANGES_Hiragana_COUNT;
            return true;
        case XR_UP_Katakana:
            *out_ranges = ranges_Katakana;
            *out_count = RANGES_Katakana_COUNT;
            return true;
        case XR_UP_Latin:
            *out_ranges = ranges_Latin;
            *out_count = RANGES_Latin_COUNT;
            return true;
        case XR_UP_Greek:
            *out_ranges = ranges_Greek;
            *out_count = RANGES_Greek_COUNT;
            return true;
        case XR_UP_Cyrillic:
            *out_ranges = ranges_Cyrillic;
            *out_count = RANGES_Cyrillic_COUNT;
            return true;
        case XR_UP_Arabic:
            *out_ranges = ranges_Arabic;
            *out_count = RANGES_Arabic_COUNT;
            return true;
        case XR_UP_Hebrew:
            *out_ranges = ranges_Hebrew;
            *out_count = RANGES_Hebrew_COUNT;
            return true;
        case XR_UP_ASCII:
            *out_ranges = ranges_ASCII;
            *out_count = RANGES_ASCII_COUNT;
            return true;
        case XR_UP_Any:
            *out_ranges = ranges_Any;
            *out_count = RANGES_Any_COUNT;
            return true;
        default:
            return false;
    }
}

bool xr_unicode_is_property(uint32_t cp, XrUnicodeProperty prop) {
    XR_DCHECK(prop >= 0 && prop < XR_UP_COUNT, "unicode_is_property: invalid property");
    const XrUnicodeRange *ranges;
    int count;

    if (!xr_unicode_property_ranges(prop, &ranges, &count)) {
        return false;
    }

    // Binary search
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < ranges[mid].lo) {
            hi = mid - 1;
        } else if (cp > ranges[mid].hi) {
            lo = mid + 1;
        } else {
            return true;  // cp in [lo, hi]
        }
    }

    return false;
}

const char *xr_unicode_property_name(XrUnicodeProperty prop) {
    for (const PropertyEntry *e = property_table; e->name != NULL; e++) {
        if (e->prop == prop)
            return e->name;
    }
    return "Unknown";
}
