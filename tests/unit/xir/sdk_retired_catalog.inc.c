/* Complete original SDK bytes retain distinct packet identities. */
typedef struct SdkRetiredPacket {
    const uint8_t *preimage, *digest, *file_digest;
    size_t length;
    const char *manifest;
    size_t manifest_length;
    uint32_t prefix[17];
} SdkRetiredPacket;
#include "sdk_retired/packet_00.inc.c"
#include "sdk_retired/packet_01.inc.c"
#include "sdk_retired/packet_02.inc.c"
#include "sdk_retired/packet_03.inc.c"
#include "sdk_retired/packet_04.inc.c"
#include "sdk_retired/packet_05.inc.c"
#include "sdk_retired/packet_06.inc.c"
#include "sdk_retired/packet_07.inc.c"
#include "sdk_retired/packet_08.inc.c"
#include "sdk_retired/packet_09.inc.c"
#include "sdk_retired/packet_10.inc.c"
#include "sdk_retired/packet_11.inc.c"
#include "sdk_retired/packet_12.inc.c"
#include "sdk_retired/packet_13.inc.c"
#include "sdk_retired/packet_14.inc.c"
#include "sdk_retired/packet_15.inc.c"
#include "sdk_retired/packet_16.inc.c"
#include "sdk_retired/packet_17.inc.c"
#include "sdk_retired/packet_18.inc.c"
#include "sdk_retired/packet_19.inc.c"
#include "sdk_retired/packet_20.inc.c"
#include "sdk_retired/packet_21.inc.c"
#include "sdk_retired/packet_22.inc.c"
#include "sdk_retired/packet_23.inc.c"
#include "sdk_retired/packet_24.inc.c"
#include "sdk_retired/packet_25.inc.c"
#include "sdk_retired/packet_26.inc.c"
#include "sdk_retired/packet_27.inc.c"
#include "sdk_retired/packet_28.inc.c"
#include "sdk_retired/packet_29.inc.c"
#include "sdk_retired/packet_30.inc.c"
#include "sdk_retired/packet_31.inc.c"
#include "sdk_retired/packet_32.inc.c"
#include "sdk_retired/packet_33.inc.c"
#include "sdk_retired/packet_34.inc.c"
#include "sdk_retired/packet_35.inc.c"
#include "sdk_retired/packet_36.inc.c"
#include "sdk_retired/packet_37.inc.c"
#include "sdk_retired/packet_38.inc.c"
#include "sdk_retired/packet_39.inc.c"
#include "sdk_retired/packet_40.inc.c"
#include "sdk_retired/packet_41.inc.c"
#include "sdk_retired/packet_42.inc.c"
#include "sdk_retired/packet_43.inc.c"
#include "sdk_retired/packet_44.inc.c"
static const SdkRetiredPacket sdk_retired_packets[] = {
    {sdk_retired_0_preimage,sdk_retired_0_digest,sdk_retired_0_file_digest,sizeof(sdk_retired_0_preimage),sdk_retired_0_manifest,sizeof(sdk_retired_0_manifest)-1,{2u,29u,74u,24u,29u,31u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_1_preimage,sdk_retired_1_digest,sdk_retired_1_file_digest,sizeof(sdk_retired_1_preimage),sdk_retired_1_manifest,sizeof(sdk_retired_1_manifest)-1,{2u,28u,88u,27u,33u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_2_preimage,sdk_retired_2_digest,sdk_retired_2_file_digest,sizeof(sdk_retired_2_preimage),sdk_retired_2_manifest,sizeof(sdk_retired_2_manifest)-1,{2u,25u,69u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_3_preimage,sdk_retired_3_digest,sdk_retired_3_file_digest,sizeof(sdk_retired_3_preimage),sdk_retired_3_manifest,sizeof(sdk_retired_3_manifest)-1,{2u,26u,71u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_4_preimage,sdk_retired_4_digest,sdk_retired_4_file_digest,sizeof(sdk_retired_4_preimage),sdk_retired_4_manifest,sizeof(sdk_retired_4_manifest)-1,{2u,28u,84u,24u,30u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_5_preimage,sdk_retired_5_digest,sdk_retired_5_file_digest,sizeof(sdk_retired_5_preimage),sdk_retired_5_manifest,sizeof(sdk_retired_5_manifest)-1,{2u,28u,74u,24u,30u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_6_preimage,sdk_retired_6_digest,sdk_retired_6_file_digest,sizeof(sdk_retired_6_preimage),sdk_retired_6_manifest,sizeof(sdk_retired_6_manifest)-1,{2u,28u,83u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_7_preimage,sdk_retired_7_digest,sdk_retired_7_file_digest,sizeof(sdk_retired_7_preimage),sdk_retired_7_manifest,sizeof(sdk_retired_7_manifest)-1,{2u,27u,72u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_8_preimage,sdk_retired_8_digest,sdk_retired_8_file_digest,sizeof(sdk_retired_8_preimage),sdk_retired_8_manifest,sizeof(sdk_retired_8_manifest)-1,{2u,28u,81u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_9_preimage,sdk_retired_9_digest,sdk_retired_9_file_digest,sizeof(sdk_retired_9_preimage),sdk_retired_9_manifest,sizeof(sdk_retired_9_manifest)-1,{2u,32u,84u,25u,29u,34u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_10_preimage,sdk_retired_10_digest,sdk_retired_10_file_digest,sizeof(sdk_retired_10_preimage),sdk_retired_10_manifest,sizeof(sdk_retired_10_manifest)-1,{2u,28u,84u,27u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_11_preimage,sdk_retired_11_digest,sdk_retired_11_file_digest,sizeof(sdk_retired_11_preimage),sdk_retired_11_manifest,sizeof(sdk_retired_11_manifest)-1,{2u,25u,64u,21u,26u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_12_preimage,sdk_retired_12_digest,sdk_retired_12_file_digest,sizeof(sdk_retired_12_preimage),sdk_retired_12_manifest,sizeof(sdk_retired_12_manifest)-1,{2u,28u,74u,24u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_13_preimage,sdk_retired_13_digest,sdk_retired_13_file_digest,sizeof(sdk_retired_13_preimage),sdk_retired_13_manifest,sizeof(sdk_retired_13_manifest)-1,{2u,23u,61u,19u,25u,28u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_14_preimage,sdk_retired_14_digest,sdk_retired_14_file_digest,sizeof(sdk_retired_14_preimage),sdk_retired_14_manifest,sizeof(sdk_retired_14_manifest)-1,{2u,28u,82u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_15_preimage,sdk_retired_15_digest,sdk_retired_15_file_digest,sizeof(sdk_retired_15_preimage),sdk_retired_15_manifest,sizeof(sdk_retired_15_manifest)-1,{2u,28u,85u,26u,30u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_16_preimage,sdk_retired_16_digest,sdk_retired_16_file_digest,sizeof(sdk_retired_16_preimage),sdk_retired_16_manifest,sizeof(sdk_retired_16_manifest)-1,{2u,31u,78u,24u,29u,33u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_17_preimage,sdk_retired_17_digest,sdk_retired_17_file_digest,sizeof(sdk_retired_17_preimage),sdk_retired_17_manifest,sizeof(sdk_retired_17_manifest)-1,{2u,25u,65u,21u,26u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_18_preimage,sdk_retired_18_digest,sdk_retired_18_file_digest,sizeof(sdk_retired_18_preimage),sdk_retired_18_manifest,sizeof(sdk_retired_18_manifest)-1,{2u,28u,86u,27u,31u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_19_preimage,sdk_retired_19_digest,sdk_retired_19_file_digest,sizeof(sdk_retired_19_preimage),sdk_retired_19_manifest,sizeof(sdk_retired_19_manifest)-1,{2u,28u,73u,23u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_20_preimage,sdk_retired_20_digest,sdk_retired_20_file_digest,sizeof(sdk_retired_20_preimage),sdk_retired_20_manifest,sizeof(sdk_retired_20_manifest)-1,{2u,33u,81u,27u,29u,36u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_21_preimage,sdk_retired_21_digest,sdk_retired_21_file_digest,sizeof(sdk_retired_21_preimage),sdk_retired_21_manifest,sizeof(sdk_retired_21_manifest)-1,{2u,28u,87u,27u,32u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_22_preimage,sdk_retired_22_digest,sdk_retired_22_file_digest,sizeof(sdk_retired_22_preimage),sdk_retired_22_manifest,sizeof(sdk_retired_22_manifest)-1,{2u,28u,80u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_23_preimage,sdk_retired_23_digest,sdk_retired_23_file_digest,sizeof(sdk_retired_23_preimage),sdk_retired_23_manifest,sizeof(sdk_retired_23_manifest)-1,{2u,28u,75u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_24_preimage,sdk_retired_24_digest,sdk_retired_24_file_digest,sizeof(sdk_retired_24_preimage),sdk_retired_24_manifest,sizeof(sdk_retired_24_manifest)-1,{2u,28u,89u,27u,34u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_25_preimage,sdk_retired_25_digest,sdk_retired_25_file_digest,sizeof(sdk_retired_25_preimage),sdk_retired_25_manifest,sizeof(sdk_retired_25_manifest)-1,{2u,28u,84u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_26_preimage,sdk_retired_26_digest,sdk_retired_26_file_digest,sizeof(sdk_retired_26_preimage),sdk_retired_26_manifest,sizeof(sdk_retired_26_manifest)-1,{2u,29u,75u,24u,29u,31u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_27_preimage,sdk_retired_27_digest,sdk_retired_27_file_digest,sizeof(sdk_retired_27_preimage),sdk_retired_27_manifest,sizeof(sdk_retired_27_manifest)-1,{2u,30u,75u,24u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_28_preimage,sdk_retired_28_digest,sdk_retired_28_file_digest,sizeof(sdk_retired_28_preimage),sdk_retired_28_manifest,sizeof(sdk_retired_28_manifest)-1,{2u,28u,76u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_29_preimage,sdk_retired_29_digest,sdk_retired_29_file_digest,sizeof(sdk_retired_29_preimage),sdk_retired_29_manifest,sizeof(sdk_retired_29_manifest)-1,{2u,32u,79u,25u,29u,34u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_30_preimage,sdk_retired_30_digest,sdk_retired_30_file_digest,sizeof(sdk_retired_30_preimage),sdk_retired_30_manifest,sizeof(sdk_retired_30_manifest)-1,{2u,25u,70u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_31_preimage,sdk_retired_31_digest,sdk_retired_31_file_digest,sizeof(sdk_retired_31_preimage),sdk_retired_31_manifest,sizeof(sdk_retired_31_manifest)-1,{2u,25u,68u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_32_preimage,sdk_retired_32_digest,sdk_retired_32_file_digest,sizeof(sdk_retired_32_preimage),sdk_retired_32_manifest,sizeof(sdk_retired_32_manifest)-1,{2u,25u,67u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_33_preimage,sdk_retired_33_digest,sdk_retired_33_file_digest,sizeof(sdk_retired_33_preimage),sdk_retired_33_manifest,sizeof(sdk_retired_33_manifest)-1,{2u,32u,80u,26u,29u,35u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_34_preimage,sdk_retired_34_digest,sdk_retired_34_file_digest,sizeof(sdk_retired_34_preimage),sdk_retired_34_manifest,sizeof(sdk_retired_34_manifest)-1,{2u,31u,79u,24u,29u,33u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_35_preimage,sdk_retired_35_digest,sdk_retired_35_file_digest,sizeof(sdk_retired_35_preimage),sdk_retired_35_manifest,sizeof(sdk_retired_35_manifest)-1,{2u,28u,79u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_36_preimage,sdk_retired_36_digest,sdk_retired_36_file_digest,sizeof(sdk_retired_36_preimage),sdk_retired_36_manifest,sizeof(sdk_retired_36_manifest)-1,{2u,25u,66u,22u,28u,29u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_37_preimage,sdk_retired_37_digest,sdk_retired_37_file_digest,sizeof(sdk_retired_37_preimage),sdk_retired_37_manifest,sizeof(sdk_retired_37_manifest)-1,{2u,28u,78u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_38_preimage,sdk_retired_38_digest,sdk_retired_38_file_digest,sizeof(sdk_retired_38_preimage),sdk_retired_38_manifest,sizeof(sdk_retired_38_manifest)-1,{2u,29u,74u,24u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_39_preimage,sdk_retired_39_digest,sdk_retired_39_file_digest,sizeof(sdk_retired_39_preimage),sdk_retired_39_manifest,sizeof(sdk_retired_39_manifest)-1,{2u,30u,77u,24u,29u,32u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_40_preimage,sdk_retired_40_digest,sdk_retired_40_file_digest,sizeof(sdk_retired_40_preimage),sdk_retired_40_manifest,sizeof(sdk_retired_40_manifest)-1,{2u,29u,76u,24u,29u,31u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_41_preimage,sdk_retired_41_digest,sdk_retired_41_file_digest,sizeof(sdk_retired_41_preimage),sdk_retired_41_manifest,sizeof(sdk_retired_41_manifest)-1,{2u,28u,73u,24u,30u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_42_preimage,sdk_retired_42_digest,sdk_retired_42_file_digest,sizeof(sdk_retired_42_preimage),sdk_retired_42_manifest,sizeof(sdk_retired_42_manifest)-1,{2u,28u,84u,26u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_43_preimage,sdk_retired_43_digest,sdk_retired_43_file_digest,sizeof(sdk_retired_43_preimage),sdk_retired_43_manifest,sizeof(sdk_retired_43_manifest)-1,{2u,28u,73u,24u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
    {sdk_retired_44_preimage,sdk_retired_44_digest,sdk_retired_44_file_digest,sizeof(sdk_retired_44_preimage),sdk_retired_44_manifest,sizeof(sdk_retired_44_manifest)-1,{2u,28u,77u,25u,29u,30u,1u,1u,1u,11u,2u,0u,1u,0u,3u,1u,1u}},
};
_Static_assert(sizeof(sdk_retired_packets)/sizeof(*sdk_retired_packets)==45,"complete retired SDK catalog");
