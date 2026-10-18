/* Complete fixed parent SDK identities remain independently rejecting. */
#define current_sdk_kat_preimage base30_75_preimage
#define current_sdk_kat_file_digest base30_75_file
#define current_sdk_kat_digest base30_75_digest
#include "retired_fixed_inputs/base_30_75/sdk_current_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage base29_74_preimage
#define current_sdk_kat_file_digest base29_74_file
#define current_sdk_kat_digest base29_74_digest
#include "retired_fixed_inputs/base_30_75/sdk_previous74_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage p09_28_85_preimage
#define current_sdk_kat_file_digest p09_28_85_file
#define current_sdk_kat_digest p09_28_85_digest
#include "retired_fixed_inputs/p09_28_85/sdk_current_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
static void sdk_fixed_parent_prefixes(XrCompileResources *resources,XrXirSdkManifest *manifest) {
    const uint8_t *preimages[]={base30_75_preimage,base29_74_preimage,p09_28_85_preimage};
    const uint8_t *digests[]={base30_75_digest,base29_74_digest,p09_28_85_digest};
    const size_t lengths[]={sizeof(base30_75_preimage),sizeof(base29_74_preimage),sizeof(p09_28_85_preimage)};
    const size_t offset=sizeof("xray:xir-runtime-sdk:v1")-1;
    XrXirSdkManifest original=*manifest;
    for (unsigned i=0;i<3;++i) {
        uint8_t hash[32];xr_sha256(preimages[i],lengths[i],hash);
        CHECK(!memcmp(hash,digests[i],32) && memcmp(hash,current_sdk_kat_digest,32));
        for (unsigned w=0;w<17;++w) {
            const uint8_t *data=preimages[i]+offset+4*w;
            manifest->prefix[w]=(uint32_t)data[0]|(uint32_t)data[1]<<8|(uint32_t)data[2]<<16|(uint32_t)data[3]<<24;
        }
        memcpy(manifest->digest,digests[i],32);XrXirSdkManifest saved=*manifest;
        XrCompileResourceStats before=sdk_stats(resources);size_t attempts=runtime_attempts;
        XrJsonCursor json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
        CHECK(!sdk_json_prefix(&json,manifest) && json.status==XR_JSON_CURSOR_INVALID);
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK(!memcmp(manifest,&saved,sizeof(saved)) && attempts==runtime_attempts);
        CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
            before.live_bytes==after.live_bytes);
    }
    *manifest=original;
    CHECK(!memcmp(base30_75_file,base29_74_file,32) && !memcmp(base30_75_file,p09_28_85_file,32));
}
