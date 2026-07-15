/* Unicode 17.0.0 grapheme generated-table verification. */

#include "../test_framework.h"
#include "shared/xr_unicode_grapheme_data.h"
#include "shared/xr_unicode_grapheme_version.h"
#include <string.h>

TEST(grapheme_table_version) {
    ASSERT_TRUE(strcmp(XR_UNICODE_GRAPHEME_VERSION, "17.0.0") == 0);
    ASSERT_EQ_INT(XR_UNICODE_GRAPHEME_UAX29_REVISION, 47);
    ASSERT_TRUE(strcmp(XR_UNICODE_GRAPHEME_PROFILE, "UAX29-C1-1") == 0);
    ASSERT_GT(XR_UNICODE_GRAPHEME_BLOCK_COUNT, 1);
}

TEST(grapheme_table_representative_properties) {
    uint8_t word;

    word = xr_unicode_grapheme_property_word(0x000d);
    ASSERT_EQ_INT(xr_grapheme_property_gcb(word), XR_GCB_CR);
    word = xr_unicode_grapheme_property_word(0x000a);
    ASSERT_EQ_INT(xr_grapheme_property_gcb(word), XR_GCB_LF);
    word = xr_unicode_grapheme_property_word(0x0308);
    ASSERT_EQ_INT(xr_grapheme_property_gcb(word), XR_GCB_EXTEND);
    word = xr_unicode_grapheme_property_word(0x200d);
    ASSERT_EQ_INT(xr_grapheme_property_gcb(word), XR_GCB_ZWJ);
    word = xr_unicode_grapheme_property_word(0x1f1fa);
    ASSERT_EQ_INT(xr_grapheme_property_gcb(word), XR_GCB_REGIONAL_INDICATOR);
    word = xr_unicode_grapheme_property_word(0x1f469);
    ASSERT_TRUE(xr_grapheme_property_is_extended_pictographic(word));
    word = xr_unicode_grapheme_property_word(0x0915);
    ASSERT_EQ_INT(xr_grapheme_property_incb(word), XR_INCB_CONSONANT);
    word = xr_unicode_grapheme_property_word(0x094d);
    ASSERT_EQ_INT(xr_grapheme_property_incb(word), XR_INCB_LINKER);
    ASSERT_EQ_INT(xr_unicode_grapheme_property_word(0x110000), 0);
}

TEST(grapheme_table_exhaustive_hash) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (uint32_t code_point = 0; code_point <= UINT32_C(0x10ffff); code_point++) {
        hash ^= xr_unicode_grapheme_property_word(code_point);
        hash *= UINT64_C(0x100000001b3);
    }
    ASSERT_EQ_UINT(hash, XR_UNICODE_GRAPHEME_PROPERTY_FNV1A64);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Unicode Grapheme - Generated Table");
RUN_TEST(grapheme_table_version);
RUN_TEST(grapheme_table_representative_properties);
RUN_TEST(grapheme_table_exhaustive_hash);

TEST_MAIN_END()
