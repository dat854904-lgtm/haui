/* SPDX-License-Identifier: GPL-2.0-only */
#include "vtxfs_csum.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/crc32c.h>
#else
#include <errno.h>
#include <string.h>
#endif

/* -------------------------------------------------------------------------
 * 1. CRC32C (Castagnoli, reflected polynomial 0x82F63B78)
 * ------------------------------------------------------------------------- */

__u32 vtx_crc32c_update(__u32 crc, const void *data, size_t len)
{
#ifdef __KERNEL__
    /* Kernel CRC32C keeps raw intermediate state. VTXFS stores the
     * complemented standard CRC, including for incremental updates. */
    return ~crc32c(~crc, data, len);
#else
    static const __u32 crc32c_nibble[16] = {
        0x00000000U, 0x105EC76FU, 0x20BD8EDEU, 0x30E349B1U,
        0x417B1DBCU, 0x5125DAD3U, 0x61C69362U, 0x7198540DU,
        0x82F63B78U, 0x92A8FC17U, 0xA24BB5A6U, 0xB21572C9U,
        0xC38D26C4U, 0xD3D3E1ABU, 0xE330A81AU, 0xF36E6F75U
    };
    const __u8 *p = (const __u8 *)data;
    size_t i;
    __u32 c = ~crc;

    for (i = 0; i < len; i++) {
        c = (c >> 4) ^ crc32c_nibble[(c ^ p[i]) & 0x0FU];
        c = (c >> 4) ^ crc32c_nibble[(c ^ (p[i] >> 4)) & 0x0FU];
    }
    return ~c;
#endif
}

__u32 vtx_crc32c(const void *data, size_t len)
{
    return vtx_crc32c_update(0U, data, len);
}

/* -------------------------------------------------------------------------
 * 2. Official BLAKE3 (unkeyed 256-bit and domain-separated 128-bit)
 * ------------------------------------------------------------------------- */

#define B3_BLOCK_LEN   64U
#define B3_CHUNK_LEN   1024U
#define B3_CHUNK_START (1U << 0)
#define B3_CHUNK_END   (1U << 1)
#define B3_PARENT      (1U << 2)
#define B3_ROOT        (1U << 3)
#define B3_MAX_DEPTH   12U

static const __u32 b3_iv[8] = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U
};

static const __u8 b3_msg_schedule[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}
};

static inline __u32 rotr32(__u32 w, unsigned int c)
{
    return (w >> c) | (w << (32U - c));
}

static inline __u32 load_le32(const __u8 *p)
{
    __le32 v;
    memcpy(&v, p, sizeof(v));
    return vtx_cpu32(v);
}

static inline void store_le32(__u8 *p, __u32 v)
{
    __le32 le = vtx_le32(v);
    memcpy(p, &le, sizeof(le));
}

static void b3_g(__u32 *s, size_t a, size_t b, size_t c, size_t d,
                 __u32 x, __u32 y)
{
    s[a] = s[a] + s[b] + x;
    s[d] = rotr32(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = rotr32(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + y;
    s[d] = rotr32(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = rotr32(s[b] ^ s[c], 7);
}

static void b3_compress(const __u32 cv[8], const __u8 block[B3_BLOCK_LEN],
                        __u8 block_len, __u64 counter, __u8 flags,
                        __u32 out[8])
{
    __u32 m[16];
    __u32 state[16];
    size_t i, r;

    for (i = 0; i < 16U; i++)
        m[i] = load_le32(block + i * 4U);

    for (i = 0; i < 8U; i++) {
        state[i] = cv[i];
        state[i + 8U] = b3_iv[i];
    }
    state[12] = (__u32)counter;
    state[13] = (__u32)(counter >> 32);
    state[14] = (__u32)block_len;
    state[15] = (__u32)flags;

    for (r = 0; r < 7U; r++) {
        const __u8 *s = b3_msg_schedule[r];
        b3_g(state, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        b3_g(state, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        b3_g(state, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        b3_g(state, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        b3_g(state, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        b3_g(state, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        b3_g(state, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        b3_g(state, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }

    for (i = 0; i < 8U; i++)
        out[i] = state[i] ^ state[i + 8U];
}

struct b3_state {
    __u32 cv_stack[B3_MAX_DEPTH][8];
    __u32 chunk_cv[8];
    __u8 buf[B3_BLOCK_LEN];
    __u64 chunk_counter;
    __u8 buf_len;
    __u8 blocks_compressed;
    __u8 stack_len;
};

static void b3_chunk_reset(struct b3_state *st, __u64 chunk_counter)
{
    memcpy(st->chunk_cv, b3_iv, sizeof(b3_iv));
    memset(st->buf, 0, sizeof(st->buf));
    st->chunk_counter = chunk_counter;
    st->buf_len = 0;
    st->blocks_compressed = 0;
}

static void b3_init(struct b3_state *st)
{
    memset(st, 0, sizeof(*st));
    b3_chunk_reset(st, 0);
}

static size_t b3_chunk_len(const struct b3_state *st)
{
    return (size_t)st->blocks_compressed * B3_BLOCK_LEN + st->buf_len;
}

static void b3_make_parent_block(const __u32 left[8], const __u32 right[8],
                                 __u8 block[B3_BLOCK_LEN])
{
    size_t i;
    for (i = 0; i < 8U; i++) {
        store_le32(block + i * 4U, left[i]);
        store_le32(block + 32U + i * 4U, right[i]);
    }
}

static void b3_add_chunk_cv(struct b3_state *st, const __u32 new_cv[8],
                            __u64 total_chunks)
{
    __u32 cv[8];
    __u8 parent_block[B3_BLOCK_LEN];

    memcpy(cv, new_cv, sizeof(cv));
    while ((total_chunks & 1ULL) == 0ULL && st->stack_len > 0U) {
        st->stack_len--;
        b3_make_parent_block(st->cv_stack[st->stack_len], cv, parent_block);
        b3_compress(b3_iv, parent_block, B3_BLOCK_LEN, 0, B3_PARENT, cv);
        total_chunks >>= 1;
    }
    if (st->stack_len < B3_MAX_DEPTH) {
        memcpy(st->cv_stack[st->stack_len], cv, sizeof(cv));
        st->stack_len++;
    }
}

static void b3_update(struct b3_state *st, const void *input, size_t len)
{
    const __u8 *p = (const __u8 *)input;

    while (len > 0U) {
        size_t take, clen = b3_chunk_len(st);
        if (clen == B3_CHUNK_LEN) {
            __u32 chunk_cv[8];
            __u8 flags = B3_CHUNK_END |
                         (st->blocks_compressed == 0U ? B3_CHUNK_START : 0U);
            __u64 total_chunks = st->chunk_counter + 1ULL;
            b3_compress(st->chunk_cv, st->buf, B3_BLOCK_LEN,
                        st->chunk_counter, flags, chunk_cv);
            b3_add_chunk_cv(st, chunk_cv, total_chunks);
            b3_chunk_reset(st, total_chunks);
            clen = 0U;
        }
        if (st->buf_len == B3_BLOCK_LEN) {
            __u8 flags = st->blocks_compressed == 0U ? B3_CHUNK_START : 0U;
            b3_compress(st->chunk_cv, st->buf, B3_BLOCK_LEN,
                        st->chunk_counter, flags, st->chunk_cv);
            st->blocks_compressed++;
            st->buf_len = 0U;
            memset(st->buf, 0, sizeof(st->buf));
        }
        take = B3_BLOCK_LEN - (size_t)st->buf_len;
        if (take > len)
            take = len;
        memcpy(st->buf + st->buf_len, p, take);
        st->buf_len = (__u8)(st->buf_len + take);
        p += take;
        len -= take;
    }
}

static void b3_finalize(struct b3_state *st, __u8 *out, size_t out_len)
{
    __u32 out_words[8];
    __u32 cv[8];
    __u8 block[B3_BLOCK_LEN];
    __u64 counter = st->chunk_counter;
    __u8 block_len = st->buf_len;
    __u8 flags = B3_CHUNK_END |
                 (st->blocks_compressed == 0U ? B3_CHUNK_START : 0U);
    size_t i;

    memcpy(cv, st->chunk_cv, sizeof(cv));
    memcpy(block, st->buf, sizeof(block));

    while (st->stack_len > 0U) {
        __u32 child_cv[8];
        b3_compress(cv, block, block_len, counter, flags, child_cv);
        st->stack_len--;
        b3_make_parent_block(st->cv_stack[st->stack_len], child_cv, block);
        memcpy(cv, b3_iv, sizeof(cv));
        counter = 0ULL;
        block_len = B3_BLOCK_LEN;
        flags = B3_PARENT;
    }
    b3_compress(cv, block, block_len, counter, (__u8)(flags | B3_ROOT),
                out_words);
    for (i = 0; i < out_len; i++) {
        __u32 w = out_words[i / 4U];
        out[i] = (__u8)(w >> ((i & 3U) * 8U));
    }
}

void vtx_blake3_256(const void *data, size_t len, __u8 out[32])
{
    struct b3_state st;
    b3_init(&st);
    if (len && data)
        b3_update(&st, data, len);
    b3_finalize(&st, out, 32U);
}

void vtx_blake3_128_domain(const __u8 domain_uuid[VTXFS_UUID_SIZE],
                           const void *data, size_t len, __u8 out[16])
{
    struct b3_state st;
    b3_init(&st);
    if (domain_uuid)
        b3_update(&st, domain_uuid, VTXFS_UUID_SIZE);
    if (len && data)
        b3_update(&st, data, len);
    b3_finalize(&st, out, 16U);
}

/* -------------------------------------------------------------------------
 * 3. SipHash-2-4 (for 48-bit directory/xattr hash and low32 disambiguator)
 * ------------------------------------------------------------------------- */

static inline __u64 rotl64(__u64 x, unsigned int b)
{
    return (x << b) | (x >> (64U - b));
}

static inline __u64 load_le64(const __u8 *p)
{
    __le64 v;
    memcpy(&v, p, sizeof(v));
    return vtx_cpu64(v);
}

#define SIPROUND(v0, v1, v2, v3)       \
    do {                               \
        (v0) += (v1);                  \
        (v1) = rotl64((v1), 13);       \
        (v1) ^= (v0);                  \
        (v0) = rotl64((v0), 32);       \
        (v2) += (v3);                  \
        (v3) = rotl64((v3), 16);       \
        (v3) ^= (v2);                  \
        (v0) += (v3);                  \
        (v3) = rotl64((v3), 21);       \
        (v3) ^= (v0);                  \
        (v2) += (v1);                  \
        (v1) = rotl64((v1), 17);       \
        (v1) ^= (v2);                  \
        (v2) = rotl64((v2), 32);       \
    } while (0)

__u64 vtx_siphash24(const __u8 seed[16], const void *data, size_t len)
{
    const __u8 *in = (const __u8 *)data;
    const __u8 *end = in + (len & ~(size_t)7U);
    __u64 k0 = seed ? load_le64(seed) : 0ULL;
    __u64 k1 = seed ? load_le64(seed + 8U) : 0ULL;
    __u64 v0 = 0x736f6d6570736575ULL ^ k0;
    __u64 v1 = 0x646f72616e646f6dULL ^ k1;
    __u64 v2 = 0x6c7967656e657261ULL ^ k0;
    __u64 v3 = 0x7465646279746573ULL ^ k1;
    __u64 b = ((__u64)len) << 56;

    for (; in != end; in += 8U) {
        __u64 m = load_le64(in);
        v3 ^= m;
        SIPROUND(v0, v1, v2, v3);
        SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    {
        size_t rem = len & 7U, i;
        for (i = 0; i < rem; i++)
            b |= ((__u64)in[i]) << (i * 8U);
    }

    v3 ^= b;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xffULL;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

void vtx_dir_name_hash(const __u8 seed[16], const void *name, size_t name_len,
                       __u64 *hash48_out, __u32 *low32_out)
{
    __u64 h = vtx_siphash24(seed, name, name_len);
    if (hash48_out)
        *hash48_out = (h >> 16) & 0xFFFFFFFFFFFFULL;
    if (low32_out)
        *low32_out = (__u32)(h & 0xFFFFFFFFULL);
}

/* -------------------------------------------------------------------------
 * 4. On-disk structure checksum sign and verify helpers
 * ------------------------------------------------------------------------- */

void vtx_super_checksum_compute(struct vtx_super_block *sb)
{
    if (!sb)
        return;
    vtx_blake3_256((const __u8 *)sb + 32U, sizeof(*sb) - 32U,
                   sb->hdr.blake3_256);
}

int vtx_super_checksum_verify(const struct vtx_super_block *sb)
{
    __u8 expected[32];
    if (!sb)
        return 0;
    vtx_blake3_256((const __u8 *)sb + 32U, sizeof(*sb) - 32U, expected);
    return memcmp(sb->hdr.blake3_256, expected, 32U) == 0;
}

void vtx_cpd_checksum_compute(struct vtx_checkpoint_desc *cpd)
{
    if (!cpd)
        return;
    vtx_blake3_256((const __u8 *)cpd + 32U, sizeof(*cpd) - 32U,
                   cpd->hdr.blake3_256);
}

int vtx_cpd_checksum_verify(const struct vtx_checkpoint_desc *cpd)
{
    __u8 expected[32];
    if (!cpd)
        return 0;
    vtx_blake3_256((const __u8 *)cpd + 32U, sizeof(*cpd) - 32U, expected);
    return memcmp(cpd->hdr.blake3_256, expected, 32U) == 0;
}

void vtx_dev_table_checksum_compute(struct vtx_dev_table *dt)
{
    if (!dt)
        return;
    vtx_blake3_256((const __u8 *)dt + 32U, sizeof(*dt) - 32U,
                   dt->hdr.checksum);
}

int vtx_dev_table_checksum_verify(const struct vtx_dev_table *dt)
{
    __u8 expected[32];
    if (!dt)
        return 0;
    vtx_blake3_256((const __u8 *)dt + 32U, sizeof(*dt) - 32U, expected);
    return memcmp(dt->hdr.checksum, expected, 32U) == 0;
}

void vtx_vag_root_checksum_compute(struct vtx_vag_dir_root *root,
                                   const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    if (!root || !metadata_uuid)
        return;
    vtx_blake3_128_domain(metadata_uuid, (const __u8 *)root + 16U,
                          sizeof(*root) - 16U, root->hdr.checksum);
}

int vtx_vag_root_checksum_verify(const struct vtx_vag_dir_root *root,
                                 const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    __u8 expected[16];
    if (!root || !metadata_uuid)
        return 0;
    vtx_blake3_128_domain(metadata_uuid, (const __u8 *)root + 16U,
                          sizeof(*root) - 16U, expected);
    return memcmp(root->hdr.checksum, expected, 16U) == 0;
}

int vtx_node_checksum_compute(void *node, size_t node_bytes,
                              const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    struct vtx_node_hdr *h = (struct vtx_node_hdr *)node;
    if (!node || !metadata_uuid ||
        !vtx_node_slots_valid(h, (__u32)node_bytes))
        return -EUCLEAN;
    vtx_blake3_128_domain(metadata_uuid, (const __u8 *)node + 16U,
                          node_bytes - 16U, h->checksum);
    return 0;
}

int vtx_node_checksum_verify(const void *node, size_t node_bytes,
                             const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)node;
    __u8 expected[16];
    if (!node || !metadata_uuid ||
        !vtx_node_slots_valid(h, (__u32)node_bytes))
        return 0;
    vtx_blake3_128_domain(metadata_uuid, (const __u8 *)node + 16U,
                          node_bytes - 16U, expected);
    return memcmp(h->checksum, expected, 16U) == 0;
}
