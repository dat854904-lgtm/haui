/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vtxfs_csum.h"
#include "../include/vtxfs_leaf.h"

static void check_hex(const __u8 *got, size_t n, const char *hex)
{
    char buf[65];
    size_t i;
    assert(n <= 32 && strlen(hex) == n * 2);
    for (i = 0; i < n; i++)
        snprintf(buf + i * 2, 3, "%02x", got[i]);
    assert(memcmp(buf, hex, n * 2) == 0);
}

int main(void)
{
    __u8 pat[8192];
    __u8 out32[32], out16_a[16], out16_b[16];
    __u8 uuid1[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    __u8 uuid2[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17};
    __u8 sip_key[16];
    __u8 sip_in[15];
    __u64 h48;
    __u32 l32;
    size_t i;
    struct vtx_super_block *sb;
    struct vtx_checkpoint_desc *cpd;
    struct vtx_dev_table *dt;
    struct vtx_vag_dir_root *vr;
    unsigned char *node;

    /* 1. CRC32C standard test vector ("123456789" -> 0xE3069283). */
    assert(vtx_crc32c("123456789", 9) == 0xE3069283U);
    assert(vtx_crc32c_update(vtx_crc32c("1234", 4), "56789", 5) == 0xE3069283U);

    /* 2. Official BLAKE3 test vectors (input[i] = i % 251). */
    for (i = 0; i < sizeof(pat); i++)
        pat[i] = (__u8)(i % 251U);

    vtx_blake3_256("", 0, out32);
    check_hex(out32, 32,
              "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    vtx_blake3_256(pat, 1, out32);
    check_hex(out32, 32,
              "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213");
    vtx_blake3_256(pat, 64, out32);
    check_hex(out32, 32,
              "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98");
    vtx_blake3_256(pat, 65, out32);
    check_hex(out32, 32,
              "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee");
    vtx_blake3_256(pat, 1024, out32);
    check_hex(out32, 32,
              "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7");
    vtx_blake3_256(pat, 1025, out32);
    check_hex(out32, 32,
              "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444");

    /* Domain separation must produce distinct 128-bit digests across UUIDs. */
    vtx_blake3_128_domain(uuid1, pat, 4080, out16_a);
    vtx_blake3_128_domain(uuid2, pat, 4080, out16_b);
    assert(memcmp(out16_a, out16_b, 16) != 0);

    /* 3. SipHash-2-4 official test vectors. */
    for (i = 0; i < 16; i++)
        sip_key[i] = (__u8)i;
    for (i = 0; i < 15; i++)
        sip_in[i] = (__u8)i;
    assert(vtx_siphash24(sip_key, "", 0) == 0x726fdb47dd0e0e31ULL);
    assert(vtx_siphash24(sip_key, sip_in, 15) == 0xa129ca6149be45e5ULL);
    vtx_dir_name_hash(sip_key, sip_in, 15, &h48, &l32);
    assert(h48 == ((0xa129ca6149be45e5ULL >> 16) & 0xFFFFFFFFFFFFULL));
    assert(l32 == 0x49be45e5U);

    /* 4. On-disk structure checksum helpers and tamper detection. */
    sb = calloc(1, sizeof(*sb));
    cpd = calloc(1, sizeof(*cpd));
    dt = calloc(1, sizeof(*dt));
    vr = calloc(1, sizeof(*vr));
    node = calloc(1, 4096);
    assert(sb && cpd && dt && vr && node);

    memcpy(sb->hdr.magic, VTXFS_MAGIC_BYTES, VTXFS_MAGIC_LEN);
    vtx_super_checksum_compute(sb);
    assert(vtx_super_checksum_verify(sb));
    sb->reserved[100] ^= 1;
    assert(!vtx_super_checksum_verify(sb));

    memcpy(cpd->hdr.magic, VTXFS_CPD_MAGIC, VTXFS_MAGIC_LEN);
    vtx_cpd_checksum_compute(cpd);
    assert(vtx_cpd_checksum_verify(cpd));
    cpd->hdr.xid = vtx_le64(1);
    assert(!vtx_cpd_checksum_verify(cpd));

    dt->hdr.magic = vtx_le32(VTXFS_DEV_TABLE_MAGIC);
    vtx_dev_table_checksum_compute(dt);
    assert(vtx_dev_table_checksum_verify(dt));
    dt->devs[0].tier_id = vtx_le16(1);
    assert(!vtx_dev_table_checksum_verify(dt));

    vr->hdr.magic = vtx_le32(VTXFS_VAG_DIR_MAGIC);
    vtx_vag_root_checksum_compute(vr, uuid1);
    assert(vtx_vag_root_checksum_verify(vr, uuid1));
    assert(!vtx_vag_root_checksum_verify(vr, uuid2));

    assert(vtx_leaf_init(node, 4096, 10, 1, 100, VTX_TREE_FS,
                         VTX_NODE_FLAG_LEAF) == 0);
    assert(vtx_node_checksum_compute(node, 4096, uuid1) == 0);
    assert(vtx_node_checksum_verify(node, 4096, uuid1));
    assert(!vtx_node_checksum_verify(node, 4096, uuid2));
    node[200] ^= 0x80;
    assert(!vtx_node_checksum_verify(node, 4096, uuid1));

    free(sb);
    free(cpd);
    free(dt);
    free(vr);
    free(node);
    puts("PASS: CRC32C, BLAKE3-256/128, SipHash-2-4 vectors and structure digests");
    return 0;
}
