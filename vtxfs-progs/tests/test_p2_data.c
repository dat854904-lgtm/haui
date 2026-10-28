/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtxfs_engine.h"

static void test_compression(void)
{
    static const __u8 algorithms[] = {
        VTX_COMPR_ZSTD, VTX_COMPR_LZ4, VTX_COMPR_ZLIB
    };
    __u8 *input = malloc(64U * 1024U);
    size_t a, i;

    assert(input != NULL);
    for (i = 0; i < 64U * 1024U; i++)
        input[i] = (__u8)((i / 128U) & 7U);

    for (a = 0; a < sizeof(algorithms); a++) {
        __u8 *frame = NULL, *output = NULL;
        __u32 frame_len = 0, output_len = 0;
        int rc = vtx_compress_extent(input, 64U * 1024U, algorithms[a],
                                     algorithms[a] == VTX_COMPR_ZSTD ? 3 : 6,
                                     &frame, &frame_len);

        assert(rc == 0 && frame != NULL && frame_len < 64U * 1024U);
        rc = vtx_decompress_extent(frame, frame_len, &output, &output_len);
        assert(rc == 0 && output_len == 64U * 1024U);
        assert(memcmp(input, output, output_len) == 0);
        vtx_buffer_free(output);

        output = NULL;
        rc = vtx_decompress_extent(frame, frame_len - 1U, &output,
                                   &output_len);
        assert(rc == -EUCLEAN && output == NULL);
        frame[frame_len - 1U] ^= 0x80U;
        rc = vtx_decompress_extent(frame, frame_len, &output, &output_len);
        assert(rc == -EUCLEAN && output == NULL);
        vtx_buffer_free(frame);
    }
    free(input);
}

static void test_crypto(void)
{
    static const __u8 algorithms[] = {
        VTX_ENC_CHACHA20_POLY1305, VTX_ENC_AES_256_GCM
    };
    __u8 key[32], wrong_key[32], plaintext[4096], ciphertext[4096];
    __u8 recovered[4096], first_ciphertext[4096], tag[16], first_tag[16];
    __u64 vblk = VTXFS_MAKE_VBLK(1, 7);
    size_t a, i;

    for (i = 0; i < sizeof(key); i++) {
        key[i] = (__u8)i;
        wrong_key[i] = (__u8)(i + 1U);
    }
    for (i = 0; i < sizeof(plaintext); i++)
        plaintext[i] = (__u8)(i * 17U + 3U);

    for (a = 0; a < sizeof(algorithms); a++) {
        int rc = vtx_encrypt_extent(algorithms[a], key, 1234, vblk, 77,
                                    plaintext, sizeof(plaintext), ciphertext,
                                    tag);
        assert(rc == 0);
        memcpy(first_ciphertext, ciphertext, sizeof(ciphertext));
        memcpy(first_tag, tag, sizeof(tag));
        rc = vtx_decrypt_extent(algorithms[a], key, 1234, vblk, 77,
                                ciphertext, sizeof(ciphertext), recovered,
                                tag);
        assert(rc == 0 && memcmp(recovered, plaintext, sizeof(plaintext)) == 0);

        rc = vtx_encrypt_extent(algorithms[a], key, 1234, vblk, 78,
                                plaintext, sizeof(plaintext), ciphertext, tag);
        assert(rc == 0);
        assert(memcmp(first_ciphertext, ciphertext, sizeof(ciphertext)) != 0 ||
               memcmp(first_tag, tag, sizeof(tag)) != 0);

        first_tag[0] ^= 1U;
        memset(recovered, 0xA5, sizeof(recovered));
        rc = vtx_decrypt_extent(algorithms[a], key, 1234, vblk, 77,
                                first_ciphertext, sizeof(first_ciphertext),
                                recovered, first_tag);
        assert(rc == -EACCES);
        first_tag[0] ^= 1U;
        rc = vtx_decrypt_extent(algorithms[a], wrong_key, 1234, vblk, 77,
                                first_ciphertext, sizeof(first_ciphertext),
                                recovered, first_tag);
        assert(rc == -EACCES);
    }

    {
        __u8 salt[16], data_key[32], wrapped[48], unwrapped[32];
        __u32 wrapped_len = 0;
        int rc;

        memset(salt, 0x31, sizeof(salt));
        memset(data_key, 0xC7, sizeof(data_key));
        rc = vtx_keybag_wrap_key(key, data_key, salt, wrapped, &wrapped_len);
        assert(rc == 0 && wrapped_len == sizeof(wrapped));
        rc = vtx_keybag_unwrap_key(key, wrapped, wrapped_len, salt, unwrapped);
        assert(rc == 0 && memcmp(unwrapped, data_key, sizeof(data_key)) == 0);
        wrapped[47] ^= 1U;
        rc = vtx_keybag_unwrap_key(key, wrapped, wrapped_len, salt, unwrapped);
        assert(rc == -EACCES);
    }
}

int main(void)
{
    test_compression();
    test_crypto();
    puts("PASS: real zstd/lz4/zlib frames and ChaCha20-Poly1305/AES-256-GCM");
    return 0;
}
