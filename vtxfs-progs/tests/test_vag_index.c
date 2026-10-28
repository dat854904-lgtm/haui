// SPDX-License-Identifier: GPL-2.0-only
/*
 * Differential test + benchmark for the word-wise VAG allocator and its
 * free-run summary index (core/vtxfs_vag.c).
 *
 * The reference implementations below are verbatim copies of the legacy
 * bit-by-bit algorithms. The new code must reproduce them EXACTLY: same
 * allocation offsets, same bitmaps, same free counters, same cursors.
 * On top of that the summary bound must never under-estimate the true
 * longest free run (an under-estimate would cause a false ENOSPC).
 *
 * Build: see tests/run_vag_index.sh
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vtxfs_engine.h"

#define NV 4U

static const __u8 UUID[VTXFS_UUID_SIZE] = { 0x11, 0x22, 0x33, 0x44, 0x55 };

static int failures;

#define CHECK(cond, ...) do {                                   \
	if (!(cond)) {                                          \
		failures++;                                     \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__);                   \
		fputc('\n', stderr);                            \
		return;                                         \
	}                                                       \
} while (0)

/* ---------------------------- RNG ---------------------------------- */
static uint64_t rng_s;
static uint64_t rnd(void)
{
	rng_s ^= rng_s << 13;
	rng_s ^= rng_s >> 7;
	rng_s ^= rng_s << 17;
	return rng_s;
}

/* ------------------- legacy reference algorithms -------------------- */
static void ref_mark(struct vtx_vag_state *s, __u32 v, __u32 off, __u32 count,
		     int allocated)
{
	__u8 *bm = s->bitmaps[v];
	__u32 free_blks = vtx_cpu32(s->descs[v].free_blocks);
	__u32 i;

	for (i = 0; i < count; i++) {
		__u32 bit = off + i;
		__u8 mask = (__u8)(1U << (bit & 7U));
		int was_set = (bm[bit >> 3] & mask) != 0;

		if (allocated && !was_set) {
			bm[bit >> 3] |= mask;
			if (free_blks > 0)
				free_blks--;
		} else if (!allocated && was_set) {
			bm[bit >> 3] &= (__u8)~mask;
			free_blks++;
			if (bit < s->alloc_cursor[v])
				s->alloc_cursor[v] = bit;
		}
	}
	s->descs[v].free_blocks = vtx_le32(free_blks);
}

/* legacy userspace allocator (cursor + 0xFF byte skip) */
static int ref_alloc(struct vtx_vag_state *s, __u32 count, __u64 *out)
{
	__u32 v;

	for (v = 0; v < s->total_vags; v++) {
		const __u8 *bm = s->bitmaps[v];
		__u32 start_off = (v == 0) ? 1U : 0U;
		__u32 cur = s->alloc_cursor[v];
		__u32 pass, ss, sl, off, run;

		if (vtx_cpu32(s->descs[v].free_blocks) < count)
			continue;
		if (cur < start_off || cur >= VTXFS_VAG_BLOCKS)
			cur = start_off;
		for (pass = 0; pass < 2; pass++) {
			if (pass == 0) {
				ss = cur;
				sl = VTXFS_VAG_BLOCKS;
			} else {
				if (cur <= start_off)
					break;
				ss = start_off;
				sl = cur + count - 1U;
				if (sl > VTXFS_VAG_BLOCKS)
					sl = VTXFS_VAG_BLOCKS;
			}
			run = 0;
			for (off = ss; off < sl; off++) {
				int used;

				if (run == 0 && (off & 7U) == 0 &&
				    bm[off >> 3] == 0xFFU) {
					off += 7U;
					continue;
				}
				used = (bm[off >> 3] >> (off & 7U)) & 1;
				if (!used) {
					if (++run == count) {
						__u32 first = off + 1U - count;

						ref_mark(s, v, first, count, 1);
						s->alloc_cursor[v] = off + 1U;
						*out = VTXFS_MAKE_VBLK(v, first);
						return 0;
					}
				} else {
					run = 0;
				}
			}
		}
	}
	return -ENOSPC;
}

/* legacy KERNEL allocator (fs/vtxfs/vag-alloc.c): no cursor, bit by bit */
static int kern_alloc(struct vtx_vag_state *s, __u32 count, __u64 *out)
{
	__u32 v, off, run, i;

	for (v = 0; v < s->total_vags; v++) {
		__u8 *bm = s->bitmaps[v];
		__u32 start_off = (v == 0) ? 1U : 0U;

		if (vtx_cpu32(s->descs[v].free_blocks) < count)
			continue;
		run = 0;
		for (off = start_off; off < VTXFS_VAG_BLOCKS; off++) {
			if (!((bm[off >> 3] >> (off & 7U)) & 1)) {
				if (++run == count) {
					__u32 first = off + 1U - count;

					for (i = 0; i < count; i++)
						bm[(first + i) >> 3] |=
							(__u8)(1U << ((first + i) & 7U));
					s->descs[v].free_blocks = vtx_le32(
						vtx_cpu32(s->descs[v].free_blocks) - count);
					*out = VTXFS_MAKE_VBLK(v, first);
					return 0;
				}
			} else {
				run = 0;
			}
		}
	}
	return -ENOSPC;
}

/* --------------------------- helpers ------------------------------- */
static void mk(struct vtx_vag_state *s)
{
	memset(s, 0, sizeof(*s));
	if (vtx_vag_init_empty(s, NV, 1, UUID) != 0) {
		fprintf(stderr, "init_empty failed\n");
		exit(2);
	}
}

static int same_state(const struct vtx_vag_state *a, const struct vtx_vag_state *b)
{
	__u32 v;

	for (v = 0; v < NV; v++) {
		if (memcmp(a->bitmaps[v], b->bitmaps[v], VTXFS_BLOCK_SIZE))
			return 0;
		if (a->descs[v].free_blocks != b->descs[v].free_blocks)
			return 0;
		if (a->alloc_cursor[v] != b->alloc_cursor[v])
			return 0;
	}
	return 1;
}

static void check_bound(const struct vtx_vag_state *a)
{
	__u32 v, runs, longest;

	for (v = 0; v < NV; v++) {
		(void)vtx_vag_free_stats(a, v, &runs, &longest);
		CHECK(a->max_free_run[v] >= longest,
		      "bound under-estimates VAG %u: bound=%u true=%u",
		      v, a->max_free_run[v], longest);
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------ differential test ------------------------- */
struct live { __u32 v, off, len; };

static __u32 pick_count(void)
{
	uint64_t r = rnd() % 100;

	if (r < 50)
		return 1;
	if (r < 80)
		return 2 + (__u32)(rnd() % 15);
	if (r < 95)
		return 17 + (__u32)(rnd() % 284);
	return 301 + (__u32)(rnd() % 5700);
}

static void diff_run(uint64_t seed, unsigned ops)
{
	struct vtx_vag_state a, b;
	struct live *lv = calloc(4096, sizeof(*lv));
	unsigned nlive = 0, i, nalloc = 0, nfree = 0, nenospc = 0;

	rng_s = seed * 0x9E3779B97F4A7C15ULL + 1;
	mk(&a);
	mk(&b);
	for (i = 0; i < ops; i++) {
		uint64_t r = rnd() % 100;

		if (r < 55 || nlive == 0) {
			__u32 cnt = pick_count();
			__u64 va = 0, vb = 0;
			int ra = vtx_vag_alloc_range(&a, cnt, &va);
			int rb = ref_alloc(&b, cnt, &vb);

			CHECK(ra == rb, "seed %llu op %u alloc(%u) ret %d vs %d",
			      (unsigned long long)seed, i, cnt, ra, rb);
			if (ra == 0) {
				CHECK(va == vb, "seed %llu op %u alloc(%u) vblk %llx vs %llx",
				      (unsigned long long)seed, i, cnt,
				      (unsigned long long)va, (unsigned long long)vb);
				if (nlive == 4096) {
					unsigned k = (unsigned)(rnd() % nlive);
					/* drop oldest bookkeeping slot: leak is fine */
					lv[k] = lv[--nlive];
				}
				lv[nlive].v = VTXFS_VBLK_TO_VAG_ID((__u64)va);
				lv[nlive].off = VTXFS_VBLK_TO_AG_OFF((__u64)va);
				lv[nlive].len = cnt;
				nlive++;
				nalloc++;
			} else {
				nenospc++;
			}
		} else if (r < 95) {
			unsigned k = (unsigned)(rnd() % nlive);
			struct live L = lv[k];
			__u32 off = L.off, len = L.len;

			if (rnd() % 4 == 0 && len > 1) {	/* partial free */
				__u32 cut = 1 + (__u32)(rnd() % (len - 1));

				off += cut;
				len -= cut;
				lv[k].len = cut;	/* keep the head live */
			} else {
				lv[k] = lv[--nlive];
			}
			CHECK(vtx_vag_mark_range(&a, VTXFS_MAKE_VBLK(L.v, off), len, 0) == 0, "free a");
			ref_mark(&b, L.v, off, len, 0);
			nfree++;
		} else {	/* random (possibly double) free / random alloc mark */
			__u32 v = (__u32)(rnd() % NV);
			__u32 len = 1 + (__u32)(rnd() % 2000);
			__u32 off = (__u32)(rnd() % (VTXFS_VAG_BLOCKS - len + 1));
			int alloc = (int)(rnd() & 1);

			if (v == 0 && off == 0)
				off = 1;
			CHECK(vtx_vag_mark_range(&a, VTXFS_MAKE_VBLK(v, off), len, alloc) == 0, "mark a");
			ref_mark(&b, v, off, len, alloc);
		}
		CHECK(same_state(&a, &b), "seed %llu op %u: state diverged",
		      (unsigned long long)seed, i);
		check_bound(&a);
		if (failures)
			break;
	}
	printf("  seed %llu: %u ops, %u allocs, %u frees, %u ENOSPC -> %s\n",
	       (unsigned long long)seed, ops, nalloc, nfree, nenospc,
	       failures ? "FAIL" : "identical to legacy");
	vtx_vag_destroy(&a);
	vtx_vag_destroy(&b);
	free(lv);
}

/* word-boundary / edge cases of the range primitive */
static void edge_cases(void)
{
	static const __u32 cases[][2] = {
		{ 1, 1 }, { 63, 2 }, { 0, 64 }, { 64, 64 }, { 60, 8 }, { 1, 127 },
		{ 100, 1 }, { 32767, 1 }, { 32704, 64 }, { 1, 32767 }, { 5, 3 },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct vtx_vag_state a, b;
		int alloc;

		mk(&a);
		mk(&b);
		for (alloc = 1; alloc >= 0; alloc--) {
			CHECK(vtx_vag_mark_range(&a, VTXFS_MAKE_VBLK(1, cases[i][0]), cases[i][1], alloc) == 0, "edge a");
			ref_mark(&b, 1, cases[i][0], cases[i][1], alloc);
			CHECK(same_state(&a, &b), "edge case off=%u len=%u alloc=%d diverged",
			      cases[i][0], cases[i][1], alloc);
		}
		vtx_vag_destroy(&a);
		vtx_vag_destroy(&b);
	}
	printf("  edge cases (word boundaries, VAG tail): identical to legacy\n");
}

/* direct bitmap writers must be able to invalidate the index safely */
static void invalidate_case(void)
{
	struct vtx_vag_state a;
	__u64 v = 0;
	__u32 i;

	mk(&a);
	for (i = 0; i < NV; i++)
		CHECK(vtx_vag_mark_range(&a, VTXFS_MAKE_VBLK(i, i == 0 ? 1 : 0),
					 VTXFS_VAG_BLOCKS - (i == 0 ? 1 : 0), 1) == 0, "fill");
	CHECK(vtx_vag_alloc_range(&a, 8, &v) == -ENOSPC, "full fs must be ENOSPC");
	/* emulate vtx_vag_rebuild_bitmaps(): rewrite bitmaps directly */
	for (i = 0; i < NV; i++) {
		memset(a.bitmaps[i], 0, VTXFS_BLOCK_SIZE);
		a.descs[i].free_blocks = vtx_le32(i == 0 ? VTXFS_VAG_BLOCKS - 1 : VTXFS_VAG_BLOCKS);
	}
	a.bitmaps[0][0] |= 1U;
	vtx_vag_summary_invalidate(&a);
	CHECK(vtx_vag_alloc_range(&a, 1000, &v) == 0,
	      "after invalidate the allocator must see the new free space");
	printf("  invalidate-after-direct-write: no false ENOSPC\n");
	vtx_vag_destroy(&a);
}

/* ---------------------------- benchmark ----------------------------- */
typedef int (*alloc_fn)(struct vtx_vag_state *, __u32, __u64 *);

static int new_alloc(struct vtx_vag_state *s, __u32 c, __u64 *o)
{
	return vtx_vag_alloc_range(s, c, o);
}

static double bench_dense(alloc_fn fn, int is_new)
{
	struct vtx_vag_state s;
	__u64 v;
	unsigned i;
	double t0;

	mk(&s);
	/* 24000 used blocks at the start of VAG 0 (like a real half-written VAG) */
	if (is_new)
		(void)vtx_vag_mark_range(&s, VTXFS_MAKE_VBLK(0, 1), 24000, 1);
	else
		ref_mark(&s, 0, 1, 24000, 1);
	t0 = now();
	for (i = 0; i < 4000; i++)
		(void)fn(&s, 1, &v);
	t0 = now() - t0;
	vtx_vag_destroy(&s);
	return t0;
}

static double bench_frag(alloc_fn fn, int is_new)
{
	struct vtx_vag_state s;
	__u64 v;
	__u32 q, b;
	unsigned i;
	double t0;

	mk(&s);
	for (q = 0; q < NV; q++) {
		__u32 st = (q == 0) ? 1U : 0U;

		if (is_new)
			(void)vtx_vag_mark_range(&s, VTXFS_MAKE_VBLK(q, st), VTXFS_VAG_BLOCKS - st, 1);
		else
			ref_mark(&s, q, st, VTXFS_VAG_BLOCKS - st, 1);
		for (b = 4; b < VTXFS_VAG_BLOCKS; b += 4) {	/* every 4th block free */
			if (is_new)
				(void)vtx_vag_mark_range(&s, VTXFS_MAKE_VBLK(q, b), 1, 0);
			else
				ref_mark(&s, q, b, 1, 0);
		}
	}
	t0 = now();
	for (i = 0; i < 2000; i++)	/* free-rich but no 64-block run exists */
		(void)fn(&s, 64, &v);
	t0 = now() - t0;
	vtx_vag_destroy(&s);
	return t0;
}

static double bench_range(int is_new)
{
	struct vtx_vag_state s;
	unsigned i;
	double t0;

	mk(&s);
	t0 = now();
	for (i = 0; i < 3000; i++) {
		if (is_new) {
			(void)vtx_vag_mark_range(&s, VTXFS_MAKE_VBLK(1, 0), 20000, 1);
			(void)vtx_vag_mark_range(&s, VTXFS_MAKE_VBLK(1, 0), 20000, 0);
		} else {
			ref_mark(&s, 1, 0, 20000, 1);
			ref_mark(&s, 1, 0, 20000, 0);
		}
	}
	t0 = now() - t0;
	vtx_vag_destroy(&s);
	return t0;
}

static void bench(void)
{
	double kd = bench_dense(kern_alloc, 0), ud = bench_dense(ref_alloc, 0),
	       nd = bench_dense(new_alloc, 1);
	double kf = bench_frag(kern_alloc, 0), uf = bench_frag(ref_alloc, 0),
	       nf = bench_frag(new_alloc, 1);
	double ur = bench_range(0), nr = bench_range(1);

	printf("\n  %-46s %12s %12s %12s %9s\n", "Scenario", "kernel-old", "user-old", "new", "vs user");
	printf("  %-46s %10.2f ms %10.2f ms %10.2f ms %8.1fx\n",
	       "S1 dense prefix: 4000 x alloc(1), 24000 used", kd * 1e3, ud * 1e3, nd * 1e3, ud / nd);
	printf("  %-46s %10.2f ms %10.2f ms %10.2f ms %8.1fx\n",
	       "S2 fragmented: 2000 x alloc(64), none fits", kf * 1e3, uf * 1e3, nf * 1e3, uf / nf);
	printf("  %-46s %12s %10.2f ms %10.2f ms %8.1fx\n",
	       "S3 range churn: 3000 x mark+free 20000 blks", "-", ur * 1e3, nr * 1e3, ur / nr);
}

int main(void)
{
	uint64_t seed;

	printf("== VAG word-wise allocator + free-run index ==\n");
	edge_cases();
	invalidate_case();
	for (seed = 1; seed <= 6 && !failures; seed++)
		diff_run(seed, 120000);
	if (failures) {
		printf("RESULT: FAIL (%d)\n", failures);
		return 1;
	}
	bench();
	printf("\nRESULT: PASS\n");
	return 0;
}
