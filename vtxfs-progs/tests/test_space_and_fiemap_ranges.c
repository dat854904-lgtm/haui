/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable Btrfs/APFS-inspired range and SFQ validation tests.
 * This exercises the SAME vtxfs_validate.h inline helpers used by kernel
 * FIEMAP and the fsck core. No kernel module or block device required.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vtxfs_validate.h"

static __u64 state = UINT64_C(0x9e3779b97f4a7c15);

static __u64 rand64(void)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

static void test_clipping(void)
{
    __u64 begin = 0, len = 0, i;
    assert(vtx_range_intersect(0, 4096, 0, 4096, &begin, &len));
    assert(begin == 0 && len == 4096);
    assert(vtx_range_intersect(100, 100, 150, 250, &begin, &len));
    assert(begin == 150 && len == 50);
    assert(vtx_range_intersect(100, 100, 50, 170, &begin, &len));
    assert(begin == 100 && len == 70);
    assert(!vtx_range_intersect(100, 100, 200, 300, &begin, &len));
    assert(!vtx_range_intersect(100, 100, 250, 250, &begin, &len));
    assert(!vtx_range_intersect(100, 0, 100, 200, &begin, &len));
    assert(!vtx_range_intersect(100, 1, 100, 200, NULL, &len));
    assert(!vtx_range_intersect(100, 1, 100, 200, &begin, NULL));
    assert(vtx_range_intersect(UINT64_MAX - 100, 200,
                               UINT64_MAX - 20, UINT64_MAX,
                               &begin, &len));
    assert(begin == UINT64_MAX - 20 && len == 20);
    for (i = 0; i < 100000; i++) {
        __u64 ext = rand64() % 1000000;
        __u64 ext_len = rand64() % 10000;
        __u64 requested = rand64() % 1000000;
        __u64 request_len = rand64() % 10000;
        __u64 expected_start = ext > requested ? ext : requested;
        __u64 ee = ext + ext_len;
        __u64 re = requested + request_len;
        __u64 expected_end = ee < re ? ee : re;
        int match = ext_len != 0 && expected_end > expected_start;
        int got = vtx_range_intersect(ext, ext_len, requested, re, &begin, &len);
        assert(got == match);
        if (got) {
            assert(begin == expected_start);
            assert(len == expected_end - expected_start);
            assert(begin >= requested);
            assert(begin + len <= re);
            assert(begin >= ext && begin + len <= ee);
        }
    }
    puts("PASS: 100000 randomized FIEMAP range intersections + overflow");
}

static void test_sfq(void)
{
    struct vtx_sfq_entry s;
    memset(&s, 0, sizeof(s));
    s.start_vblk = vtx_le64(1);
    s.block_count = vtx_le64(1);
    assert(vtx_sfq_range_valid(&s, 1));
    s.start_vblk = vtx_le64(VTXFS_MAKE_VBLK(0, 32767));
    assert(vtx_sfq_range_valid(&s, 1));
    s.block_count = vtx_le64(2);
    assert(!vtx_sfq_range_valid(&s, 1)); /* boundary crossed */
    s.start_vblk = vtx_le64(VTXFS_MAKE_VBLK(1, 0));
    s.block_count = vtx_le64(32768);
    assert(vtx_sfq_range_valid(&s, 2));
    assert(!vtx_sfq_range_valid(&s, 1)); /* beyond mounted VAGs */
    s.start_vblk = vtx_le64(0);
    assert(!vtx_sfq_range_valid(&s, 2)); /* VBLK 0 reserved */
    s.start_vblk = vtx_le64(VTXFS_MAKE_VBLK(1, 0));
    s.block_count = vtx_le64(0);
    assert(!vtx_sfq_range_valid(&s, 2));
    s.block_count = vtx_le64(UINT64_C(0x100000001));
    assert(!vtx_sfq_range_valid(&s, 2)); /* u64 -> u32 truncation */
    s.block_count = vtx_le64(65536);
    assert(!vtx_sfq_range_valid(&s, 2));
    s.block_count = vtx_le64(1);
    assert(!vtx_sfq_range_valid(&s, 0));
    assert(!vtx_sfq_range_valid(NULL, 2));
    puts("PASS: SFQ invalid length, VAG boundary and u64 truncation guards");
}

int main(void)
{
    test_clipping();
    test_sfq();
    return 0;
}
