/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Standalone AEAD helpers for the future encrypted-extent protocol.
 *
 * Unlike the earlier proposal, AES-GCM is AES-GCM and Poly1305 is not
 * replaced by SipHash.  The on-disk AEAD feature remains gated off until
 * nonce allocation, rollback handling, and key lifecycle are specified.
 */
#include "vtxfs_engine.h"

#ifdef __KERNEL__
#include <crypto/aead.h>
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#endif

#define VTX_AEAD_NONCE_BYTES 12U
#define VTX_AEAD_TAG_BYTES   16U
#define VTX_AEAD_MAX_INPUT   VTXFS_MAX_COMPRESSED_FRAME_BYTES
#define VTX_AEAD_AAD_BYTES   32U

static void secure_clear(void *ptr, size_t len)
{
#ifdef __KERNEL__
    memzero_explicit(ptr, len);
#else
    OPENSSL_cleanse(ptr, len);
#endif
}

static void store_le64(__u8 out[8], __u64 value)
{
    __le64 le = vtx_le64(value);

    memcpy(out, &le, sizeof(le));
}

static void build_extent_aad(__u8 aad[VTX_AEAD_AAD_BYTES], __u8 enc_type,
                             __u64 allocation_id, __u64 vblk,
                             __u64 birth_xid)
{
    memset(aad, 0, VTX_AEAD_AAD_BYTES);
    store_le64(aad, allocation_id);
    store_le64(aad + 8, vblk);
    store_le64(aad + 16, birth_xid);
    aad[24] = enc_type;
    aad[25] = 1; /* AAD serialization version */
}

static void derive_extent_nonce(__u8 nonce[VTX_AEAD_NONCE_BYTES],
                                const __u8 key[32], __u8 enc_type,
                                const __u8 aad[VTX_AEAD_AAD_BYTES])
{
    __u8 material[80];
    __u8 digest[32];

    memset(material, 0, sizeof(material));
    memcpy(material, "VTXFS-AEAD-v1", 13);
    material[15] = enc_type;
    memcpy(material + 16, key, 32);
    memcpy(material + 48, aad, VTX_AEAD_AAD_BYTES);
    vtx_blake3_256(material, sizeof(material), digest);
    memcpy(nonce, digest, VTX_AEAD_NONCE_BYTES);
    secure_clear(digest, sizeof(digest));
    secure_clear(material, sizeof(material));
}

static void derive_wrap_nonce(__u8 nonce[VTX_AEAD_NONCE_BYTES],
                              const __u8 master_key[32],
                              const __u8 salt[16])
{
    __u8 material[64];
    __u8 digest[32];

    memset(material, 0, sizeof(material));
    memcpy(material, "VTXFS-WRAP-v1", 13);
    memcpy(material + 16, master_key, 32);
    memcpy(material + 48, salt, 16);
    vtx_blake3_256(material, sizeof(material), digest);
    memcpy(nonce, digest, VTX_AEAD_NONCE_BYTES);
    secure_clear(digest, sizeof(digest));
    secure_clear(material, sizeof(material));
}

#ifdef __KERNEL__
static const char *kernel_aead_name(__u8 enc_type)
{
    switch (enc_type) {
    case VTX_ENC_CHACHA20_POLY1305:
        return "rfc7539(chacha20,poly1305)";
    case VTX_ENC_AES_256_GCM:
        return "gcm(aes)";
    default:
        return NULL;
    }
}

static int aead_crypt(int encrypt, __u8 enc_type, const __u8 key[32],
                      __u8 nonce[VTX_AEAD_NONCE_BYTES],
                      const __u8 *aad, size_t aad_len,
                      const __u8 *input, size_t input_len,
                      __u8 *output, __u8 tag[VTX_AEAD_TAG_BYTES])
{
    const char *name = kernel_aead_name(enc_type);
    struct crypto_aead *tfm;
    struct aead_request *request = NULL;
    struct scatterlist sg;
    DECLARE_CRYPTO_WAIT(wait);
    size_t buffer_len = aad_len + input_len + VTX_AEAD_TAG_BYTES;
    __u8 *buffer = NULL;
    int ret;

    if (!name)
        return -EOPNOTSUPP;
    tfm = crypto_alloc_aead(name, 0, 0);
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    ret = crypto_aead_setkey(tfm, key, 32);
    if (ret)
        goto out;
    ret = crypto_aead_setauthsize(tfm, VTX_AEAD_TAG_BYTES);
    if (ret)
        goto out;

    /* sg_init_one() requires direct-mapped memory, hence kzalloc here. */
    buffer = kzalloc(buffer_len, GFP_NOFS);
    if (!buffer) {
        ret = -ENOMEM;
        goto out;
    }
    memcpy(buffer, aad, aad_len);
    memcpy(buffer + aad_len, input, input_len);
    if (!encrypt)
        memcpy(buffer + aad_len + input_len, tag, VTX_AEAD_TAG_BYTES);

    request = aead_request_alloc(tfm, GFP_NOFS);
    if (!request) {
        ret = -ENOMEM;
        goto out;
    }
    sg_init_one(&sg, buffer, buffer_len);
    aead_request_set_callback(request,
                              CRYPTO_TFM_REQ_MAY_BACKLOG |
                                  CRYPTO_TFM_REQ_MAY_SLEEP,
                              crypto_req_done, &wait);
    aead_request_set_ad(request, aad_len);
    aead_request_set_crypt(request, &sg, &sg,
                           input_len + (encrypt ? 0 : VTX_AEAD_TAG_BYTES),
                           nonce);
    ret = crypto_wait_req(encrypt ? crypto_aead_encrypt(request)
                                  : crypto_aead_decrypt(request),
                          &wait);
    if (ret)
        goto out;
    memcpy(output, buffer + aad_len, input_len);
    if (encrypt)
        memcpy(tag, buffer + aad_len + input_len, VTX_AEAD_TAG_BYTES);
out:
    if (buffer) {
        secure_clear(buffer, buffer_len);
        kfree(buffer);
    }
    aead_request_free(request);
    crypto_free_aead(tfm);
    return ret == -EBADMSG ? -EACCES : ret;
}
#else
static const EVP_CIPHER *userspace_cipher(__u8 enc_type)
{
    switch (enc_type) {
    case VTX_ENC_CHACHA20_POLY1305:
        return EVP_chacha20_poly1305();
    case VTX_ENC_AES_256_GCM:
        return EVP_aes_256_gcm();
    default:
        return NULL;
    }
}

static int aead_crypt(int encrypt, __u8 enc_type, const __u8 key[32],
                      __u8 nonce[VTX_AEAD_NONCE_BYTES],
                      const __u8 *aad, size_t aad_len,
                      const __u8 *input, size_t input_len,
                      __u8 *output, __u8 tag[VTX_AEAD_TAG_BYTES])
{
    const EVP_CIPHER *cipher = userspace_cipher(enc_type);
    EVP_CIPHER_CTX *ctx;
    int chunk = 0, final_len = 0;
    int ok;

    if (!cipher)
        return -EOPNOTSUPP;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -ENOMEM;
    if (encrypt) {
        ok = EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                 VTX_AEAD_NONCE_BYTES, NULL) == 1 &&
             EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
             EVP_EncryptUpdate(ctx, NULL, &chunk, aad, (int)aad_len) == 1 &&
             EVP_EncryptUpdate(ctx, output, &chunk, input,
                               (int)input_len) == 1 &&
             EVP_EncryptFinal_ex(ctx, output + chunk, &final_len) == 1 &&
             (size_t)(chunk + final_len) == input_len &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                                 VTX_AEAD_TAG_BYTES, tag) == 1;
    } else {
        ok = EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                 VTX_AEAD_NONCE_BYTES, NULL) == 1 &&
             EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
             EVP_DecryptUpdate(ctx, NULL, &chunk, aad, (int)aad_len) == 1 &&
             EVP_DecryptUpdate(ctx, output, &chunk, input,
                               (int)input_len) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                                 VTX_AEAD_TAG_BYTES, tag) == 1 &&
             EVP_DecryptFinal_ex(ctx, output + chunk, &final_len) == 1 &&
             (size_t)(chunk + final_len) == input_len;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok && !encrypt)
        secure_clear(output, input_len);
    return ok ? 0 : (encrypt ? -EIO : -EACCES);
}
#endif

int vtx_encrypt_extent(__u8 enc_type, const __u8 key[32],
                       __u64 allocation_id, __u64 vblk, __u64 birth_xid,
                       const __u8 *plaintext, __u32 len,
                       __u8 *ciphertext, __u8 tag[16])
{
    __u8 aad[VTX_AEAD_AAD_BYTES];
    __u8 nonce[VTX_AEAD_NONCE_BYTES];
    int ret;

    if (!key || !plaintext || !ciphertext || !tag || !len ||
        len > VTX_AEAD_MAX_INPUT || !allocation_id ||
        !VTXFS_VALID_VBLK(vblk) || !birth_xid)
        return -EINVAL;
    if (enc_type != VTX_ENC_CHACHA20_POLY1305 &&
        enc_type != VTX_ENC_AES_256_GCM)
        return -EOPNOTSUPP;
    build_extent_aad(aad, enc_type, allocation_id, vblk, birth_xid);
    derive_extent_nonce(nonce, key, enc_type, aad);
    ret = aead_crypt(1, enc_type, key, nonce, aad, sizeof(aad), plaintext,
                     len, ciphertext, tag);
    secure_clear(nonce, sizeof(nonce));
    secure_clear(aad, sizeof(aad));
    return ret;
}

int vtx_decrypt_extent(__u8 enc_type, const __u8 key[32],
                       __u64 allocation_id, __u64 vblk, __u64 birth_xid,
                       const __u8 *ciphertext, __u32 len,
                       __u8 *plaintext, const __u8 tag[16])
{
    __u8 aad[VTX_AEAD_AAD_BYTES];
    __u8 nonce[VTX_AEAD_NONCE_BYTES];
    __u8 tag_copy[VTX_AEAD_TAG_BYTES];
    int ret;

    if (!key || !ciphertext || !plaintext || !tag || !len ||
        len > VTX_AEAD_MAX_INPUT || !allocation_id ||
        !VTXFS_VALID_VBLK(vblk) || !birth_xid)
        return -EINVAL;
    if (enc_type != VTX_ENC_CHACHA20_POLY1305 &&
        enc_type != VTX_ENC_AES_256_GCM)
        return -EOPNOTSUPP;
    build_extent_aad(aad, enc_type, allocation_id, vblk, birth_xid);
    derive_extent_nonce(nonce, key, enc_type, aad);
    memcpy(tag_copy, tag, sizeof(tag_copy));
    ret = aead_crypt(0, enc_type, key, nonce, aad, sizeof(aad), ciphertext,
                     len, plaintext, tag_copy);
    secure_clear(tag_copy, sizeof(tag_copy));
    secure_clear(nonce, sizeof(nonce));
    secure_clear(aad, sizeof(aad));
    return ret;
}

int vtx_keybag_wrap_key(const __u8 master_key[32], const __u8 data_key[32],
                        const __u8 salt[16], __u8 wrapped_key[48],
                        __u32 *wrapped_len)
{
    static const __u8 aad[] = "VTXFS key wrap v1";
    __u8 nonce[VTX_AEAD_NONCE_BYTES];
    __u8 tag[VTX_AEAD_TAG_BYTES];
    int ret;

    if (!master_key || !data_key || !salt || !wrapped_key || !wrapped_len)
        return -EINVAL;
    *wrapped_len = 0;
    derive_wrap_nonce(nonce, master_key, salt);
    ret = aead_crypt(1, VTX_ENC_CHACHA20_POLY1305, master_key, nonce,
                     aad, sizeof(aad) - 1U, data_key, 32, wrapped_key, tag);
    if (!ret) {
        memcpy(wrapped_key + 32, tag, sizeof(tag));
        *wrapped_len = 48;
    } else {
        secure_clear(wrapped_key, 48);
    }
    secure_clear(tag, sizeof(tag));
    secure_clear(nonce, sizeof(nonce));
    return ret;
}

int vtx_keybag_unwrap_key(const __u8 master_key[32],
                          const __u8 wrapped_key[48], __u32 wrapped_len,
                          const __u8 salt[16], __u8 data_key[32])
{
    static const __u8 aad[] = "VTXFS key wrap v1";
    __u8 nonce[VTX_AEAD_NONCE_BYTES];
    __u8 tag[VTX_AEAD_TAG_BYTES];
    int ret;

    if (!master_key || !wrapped_key || !salt || !data_key ||
        wrapped_len != 48)
        return -EINVAL;
    derive_wrap_nonce(nonce, master_key, salt);
    memcpy(tag, wrapped_key + 32, sizeof(tag));
    ret = aead_crypt(0, VTX_ENC_CHACHA20_POLY1305, master_key, nonce,
                     aad, sizeof(aad) - 1U, wrapped_key, 32, data_key, tag);
    if (ret)
        secure_clear(data_key, 32);
    secure_clear(tag, sizeof(tag));
    secure_clear(nonce, sizeof(nonce));
    return ret;
}

#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(vtx_encrypt_extent);
EXPORT_SYMBOL_GPL(vtx_decrypt_extent);
EXPORT_SYMBOL_GPL(vtx_keybag_wrap_key);
EXPORT_SYMBOL_GPL(vtx_keybag_unwrap_key);
#endif
