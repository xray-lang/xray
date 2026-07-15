#include "xr_unicode_grapheme_data.h"

#include "xr_unicode_grapheme_data.inc"

uint8_t xr_unicode_grapheme_property_word(uint32_t code_point) {
    uint32_t page;
    uint16_t block;

    if (code_point > UINT32_C(0x10ffff))
        return 0;
    page = code_point >> 8;
    block = xr_grapheme_page_to_block[page];
    return xr_grapheme_property_blocks[block][code_point & UINT32_C(0xff)];
}
