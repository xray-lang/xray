/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto_suite_inventory.h - Exact original seven-file crypto test catalog
 *
 * KEY CONCEPT:
 *   Expected names come from the original full inputs, independently of the
 *   received catalog. A module initializer never substitutes for a test body.
 */
#ifndef CRYPTO_SUITE_INVENTORY_H
#define CRYPTO_SUITE_INVENTORY_H
#include <stdint.h>
typedef struct CryptoSuite { const char *path,*prefix; uint32_t count; const char *names[14]; } CryptoSuite;
static const CryptoSuite crypto_suites[7]={
    {"tests/regression/10_stdlib/1400_crypto_hash.xr","crypto_suite_0",14,{"test_md5_empty_string","test_md5_hello","test_md5_hello_world","test_md5_numbers","test_md5_long_string","test_sha1_empty_string","test_sha1_hello","test_sha1_hello_world","test_sha1_long_string","test_sha256_empty_string","test_sha256_hello","test_sha256_hello_world","test_sha256_long_string","test_hash_lengths"}},
    {"tests/regression/10_stdlib/1401_crypto_hmac.xr","crypto_suite_1",12,{"test_hmac_sha256_basic","test_hmac_sha256_empty_message","test_hmac_sha256_complex_key","test_hmac_sha256_rfc4231_jefe_vector","test_hmac_sha256_long_data","test_hmac_sha1_basic","test_hmac_sha1_empty_message","test_hmac_sha1_length","test_hmac_md5_basic","test_hmac_md5_empty_message","test_hmac_md5_length","test_hmac_unsupported_algorithm_returns_null"}},
    {"tests/regression/10_stdlib/1402_crypto_random.xr","crypto_suite_2",10,{"test_random_bytes_length_8","test_random_bytes_length_16","test_random_bytes_length_32","test_random_bytes_uniqueness","test_random_bytes_hex_format","test_uuid_length","test_uuid_format","test_uuid_parts_length","test_uuid_uniqueness","test_uuid_multiple_uniqueness"}},
    {"tests/regression/10_stdlib/1403_crypto_sha512.xr","crypto_suite_3",8,{"test_sha512_empty_string","test_sha512_hello","test_sha512_hello_world","test_sha512_length","test_sha512_long_string","test_hmac_sha512_basic","test_hmac_sha512_empty_message","test_hmac_sha512_known_vector"}},
    {"tests/regression/10_stdlib/1404_crypto_aes.xr","crypto_suite_4",8,{"test_encrypt_decrypt_basic","test_encrypt_decrypt_empty","test_encrypt_decrypt_long_text","test_encrypt_output_is_hex","test_encrypt_different_each_time","test_decrypt_wrong_key","test_encrypt_decrypt_exactly_16_bytes","test_encrypt_decrypt_unicode"}},
    {"tests/regression/10_stdlib/1405_crypto_utils.xr","crypto_suite_5",8,{"test_timing_safe_equal_same","test_timing_safe_equal_different","test_timing_safe_equal_different_length","test_timing_safe_equal_empty","test_timing_safe_equal_hmac_verify","test_timing_safe_equal_hmac_mismatch","test_all_hash_lengths","test_all_hmac_lengths"}},
    {"tests/regression/10_stdlib/1406_crypto_aead.xr","crypto_suite_6",7,{"test_reference_wire_forms_decrypt_to_their_plaintext","test_every_padding_length_round_trips","test_a_different_key_does_not_authenticate","test_two_encryptions_of_one_plaintext_differ","test_tampering_anywhere_is_refused","test_a_wire_form_of_the_wrong_length_is_a_typed_error","test_a_non_hex_wire_form_answers_null"}},
};
#endif // CRYPTO_SUITE_INVENTORY_H
