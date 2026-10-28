/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Standalone compressed-frame codec for the future P2 extent path.
 *
 * The payloads are real Zstandard, LZ4 block, and zlib streams.  This file
 * does not alter vtx_extent_item or enable VTXFS_FEAT_INCOMPAT_COMPRESS;
 * doing that requires a separately crash-tested read/write protocol.
 */
#include "vtxfs_engine.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/lz4.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>
#include <linux/zstd.h>
#else
#include <errno.h>
#include <limits.h>
#include <lz4.h>
#include <lz4hc.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>
#endif

#define VTX_COMPRESS_MAGIC       0x46435456U /* "VTCF" little-endian */
#define VTX_COMPRESS_VERSION     1U
#define VTX_COMPRESS_FRAME_MAX   VTXFS_MAX_COMPRESSED_FRAME_BYTES

struct vtx_compressed_frame_hdr {
    __le32 magic;
    __u8 version;
    __u8 algorithm;
    __u8 level;
    __u8 flags;
    __le16 header_bytes;
    __le16 reserved;
    __le32 logical_bytes;
    __le32 compressed_bytes;
    __le32 crc32c_uncompressed;
} __packed;

VTXFS_STATIC_ASSERT(sizeof(struct vtx_compressed_frame_hdr) == 24,
                    "compressed frame header must be 24 bytes");

static void *vtx_codec_zalloc(size_t size)
{
#ifdef __KERNEL__
    return kvzalloc(size, GFP_NOFS);
#else
    return calloc(1, size);
#endif
}

void vtx_buffer_free(void *buffer)
{
#ifdef __KERNEL__
    kvfree(buffer);
#else
    free(buffer);
#endif
}

static int codec_level_valid(__u8 algorithm, __u8 level)
{
    switch (algorithm) {
    case VTX_COMPR_ZSTD:
        return level >= 1U && level <= 22U;
    case VTX_COMPR_LZ4:
        return level >= 1U && level <= 12U;
    case VTX_COMPR_ZLIB:
        return level >= 1U && level <= 9U;
    default:
        return 0;
    }
}

static size_t codec_bound(__u8 algorithm, size_t input_len)
{
    switch (algorithm) {
    case VTX_COMPR_ZSTD:
#ifdef __KERNEL__
        return zstd_compress_bound(input_len);
#else
        return ZSTD_compressBound(input_len);
#endif
    case VTX_COMPR_LZ4:
        return (size_t)LZ4_compressBound(input_len);
    case VTX_COMPR_ZLIB:
#ifdef __KERNEL__
        return (size_t)deflateBound((unsigned long)input_len);
#else
        return (size_t)compressBound((uLong)input_len);
#endif
    default:
        return 0;
    }
}

#ifdef __KERNEL__
static size_t codec_compress_zstd(const void *src, size_t src_len, void *dst,
                                  size_t dst_cap, int level)
{
    zstd_parameters params = zstd_get_params(level, src_len);
    size_t workspace_len = zstd_cctx_workspace_bound(&params.cParams);
    void *workspace = vtx_codec_zalloc(workspace_len);
    zstd_cctx *ctx;
    size_t out;

    if (!workspace)
        return 0;
    ctx = zstd_init_cctx(workspace, workspace_len);
    if (!ctx) {
        vtx_buffer_free(workspace);
        return 0;
    }
    out = zstd_compress_cctx(ctx, dst, dst_cap, src, src_len, &params);
    vtx_buffer_free(workspace);
    return zstd_is_error(out) ? 0 : out;
}

static size_t codec_decompress_zstd(const void *src, size_t src_len,
                                    void *dst, size_t dst_cap)
{
    size_t workspace_len = zstd_dctx_workspace_bound();
    void *workspace = vtx_codec_zalloc(workspace_len);
    zstd_dctx *ctx;
    size_t out;

    if (!workspace)
        return 0;
    ctx = zstd_init_dctx(workspace, workspace_len);
    if (!ctx) {
        vtx_buffer_free(workspace);
        return 0;
    }
    out = zstd_decompress_dctx(ctx, dst, dst_cap, src, src_len);
    vtx_buffer_free(workspace);
    return zstd_is_error(out) ? 0 : out;
}

static size_t codec_compress_lz4(const void *src, size_t src_len, void *dst,
                                 size_t dst_cap, int level)
{
    size_t work_len = level > 1 ? LZ4HC_MEM_COMPRESS : LZ4_MEM_COMPRESS;
    void *work = vtx_codec_zalloc(work_len);
    int out;

    if (!work)
        return 0;
    if (level > 1)
        out = LZ4_compress_HC(src, dst, (int)src_len, (int)dst_cap,
                              level, work);
    else
        out = LZ4_compress_default(src, dst, (int)src_len,
                                   (int)dst_cap, work);
    vtx_buffer_free(work);
    return out > 0 ? (size_t)out : 0;
}

static size_t codec_compress_zlib(const void *src, size_t src_len, void *dst,
                                  size_t dst_cap, int level)
{
    z_stream stream;
    int ret;

    memset(&stream, 0, sizeof(stream));
    stream.workspace = vtx_codec_zalloc(
        (size_t)zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL));
    if (!stream.workspace)
        return 0;
    stream.next_in = (unsigned char *)src;
    stream.avail_in = (unsigned int)src_len;
    stream.next_out = dst;
    stream.avail_out = (unsigned int)dst_cap;
    ret = zlib_deflateInit(&stream, level);
    if (ret == Z_OK)
        ret = zlib_deflate(&stream, Z_FINISH);
    if (ret == Z_STREAM_END)
        ret = zlib_deflateEnd(&stream);
    else
        (void)zlib_deflateEnd(&stream);
    vtx_buffer_free(stream.workspace);
    return ret == Z_OK ? (size_t)stream.total_out : 0;
}

static size_t codec_decompress_zlib(const void *src, size_t src_len,
                                    void *dst, size_t dst_cap)
{
    z_stream stream;
    int ret;

    memset(&stream, 0, sizeof(stream));
    stream.workspace = vtx_codec_zalloc((size_t)zlib_inflate_workspacesize());
    if (!stream.workspace)
        return 0;
    stream.next_in = (unsigned char *)src;
    stream.avail_in = (unsigned int)src_len;
    stream.next_out = dst;
    stream.avail_out = (unsigned int)dst_cap;
    ret = zlib_inflateInit(&stream);
    if (ret == Z_OK)
        ret = zlib_inflate(&stream, Z_FINISH);
    if (ret == Z_STREAM_END && stream.total_in == src_len)
        ret = zlib_inflateEnd(&stream);
    else {
        (void)zlib_inflateEnd(&stream);
        ret = Z_DATA_ERROR;
    }
    vtx_buffer_free(stream.workspace);
    return ret == Z_OK ? (size_t)stream.total_out : 0;
}
#else
static size_t codec_compress_zstd(const void *src, size_t src_len, void *dst,
                                  size_t dst_cap, int level)
{
    size_t out = ZSTD_compress(dst, dst_cap, src, src_len, level);

    return ZSTD_isError(out) ? 0 : out;
}

static size_t codec_decompress_zstd(const void *src, size_t src_len,
                                    void *dst, size_t dst_cap)
{
    size_t out = ZSTD_decompress(dst, dst_cap, src, src_len);

    return ZSTD_isError(out) ? 0 : out;
}

static size_t codec_compress_lz4(const void *src, size_t src_len, void *dst,
                                 size_t dst_cap, int level)
{
    int out;

    if (level > 1)
        out = LZ4_compress_HC((const char *)src, (char *)dst, (int)src_len,
                              (int)dst_cap, level);
    else
        out = LZ4_compress_default((const char *)src, (char *)dst,
                                   (int)src_len, (int)dst_cap);
    return out > 0 ? (size_t)out : 0;
}

static size_t codec_compress_zlib(const void *src, size_t src_len, void *dst,
                                  size_t dst_cap, int level)
{
    uLongf out = (uLongf)dst_cap;
    int ret = compress2(dst, &out, src, (uLong)src_len, level);

    return ret == Z_OK ? (size_t)out : 0;
}

static size_t codec_decompress_zlib(const void *src, size_t src_len,
                                    void *dst, size_t dst_cap)
{
    uLongf out = (uLongf)dst_cap;
    int ret = uncompress(dst, &out, src, (uLong)src_len);

    return ret == Z_OK ? (size_t)out : 0;
}
#endif

static size_t codec_compress(__u8 algorithm, const void *src, size_t src_len,
                             void *dst, size_t dst_cap, int level)
{
    switch (algorithm) {
    case VTX_COMPR_ZSTD:
        return codec_compress_zstd(src, src_len, dst, dst_cap, level);
    case VTX_COMPR_LZ4:
        return codec_compress_lz4(src, src_len, dst, dst_cap, level);
    case VTX_COMPR_ZLIB:
        return codec_compress_zlib(src, src_len, dst, dst_cap, level);
    default:
        return 0;
    }
}

static size_t codec_decompress(__u8 algorithm, const void *src,
                               size_t src_len, void *dst, size_t dst_cap)
{
    switch (algorithm) {
    case VTX_COMPR_ZSTD:
        return codec_decompress_zstd(src, src_len, dst, dst_cap);
    case VTX_COMPR_LZ4: {
        int out = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      (int)src_len, (int)dst_cap);
        return out > 0 ? (size_t)out : 0;
    }
    case VTX_COMPR_ZLIB:
        return codec_decompress_zlib(src, src_len, dst, dst_cap);
    default:
        return 0;
    }
}

int vtx_compress_extent(const void *input, __u32 input_len,
                        __u8 algorithm, __u8 level,
                        __u8 **output_out, __u32 *output_len_out)
{
    struct vtx_compressed_frame_hdr *hdr;
    size_t bound, compressed_len, frame_len;
    __u8 *frame;

    if (!output_out || !output_len_out)
        return -EINVAL;
    *output_out = NULL;
    *output_len_out = 0;
    if (!input || !input_len || input_len > VTX_COMPRESS_FRAME_MAX ||
        !codec_level_valid(algorithm, level))
        return -EINVAL;

    bound = codec_bound(algorithm, input_len);
    if (!bound || bound > (__u32)-1 - sizeof(*hdr))
        return -EOVERFLOW;
    frame_len = sizeof(*hdr) + bound;
    frame = vtx_codec_zalloc(frame_len);
    if (!frame)
        return -ENOMEM;

    compressed_len = codec_compress(algorithm, input, input_len,
                                    frame + sizeof(*hdr), bound, level);
    if (!compressed_len || compressed_len >= input_len) {
        vtx_buffer_free(frame);
        return -ENOSPC;
    }

    hdr = (struct vtx_compressed_frame_hdr *)frame;
    hdr->magic = vtx_le32(VTX_COMPRESS_MAGIC);
    hdr->version = VTX_COMPRESS_VERSION;
    hdr->algorithm = algorithm;
    hdr->level = level;
    hdr->header_bytes = vtx_le16((__u16)sizeof(*hdr));
    hdr->logical_bytes = vtx_le32(input_len);
    hdr->compressed_bytes = vtx_le32((__u32)compressed_len);
    hdr->crc32c_uncompressed = vtx_le32(vtx_crc32c(input, input_len));
    *output_out = frame;
    *output_len_out = (__u32)(sizeof(*hdr) + compressed_len);
    return 0;
}

int vtx_decompress_extent(const void *input, __u32 input_len,
                          __u8 **output_out, __u32 *output_len_out)
{
    const struct vtx_compressed_frame_hdr *hdr;
    __u32 logical_len, compressed_len;
    size_t produced;
    __u8 *output;

    if (!output_out || !output_len_out)
        return -EINVAL;
    *output_out = NULL;
    *output_len_out = 0;
    if (!input || input_len < sizeof(*hdr))
        return -EINVAL;

    hdr = (const struct vtx_compressed_frame_hdr *)input;
    logical_len = vtx_cpu32(hdr->logical_bytes);
    compressed_len = vtx_cpu32(hdr->compressed_bytes);
    if (vtx_cpu32(hdr->magic) != VTX_COMPRESS_MAGIC ||
        hdr->version != VTX_COMPRESS_VERSION || hdr->flags != 0 ||
        vtx_cpu16(hdr->header_bytes) != sizeof(*hdr) ||
        vtx_cpu16(hdr->reserved) != 0 ||
        !codec_level_valid(hdr->algorithm, hdr->level) || !logical_len ||
        logical_len > VTX_COMPRESS_FRAME_MAX || !compressed_len ||
        compressed_len >= logical_len ||
        input_len < sizeof(*hdr) + compressed_len)
        return -EUCLEAN;

    output = vtx_codec_zalloc(logical_len);
    if (!output)
        return -ENOMEM;
    produced = codec_decompress(hdr->algorithm,
                                (const __u8 *)input + sizeof(*hdr),
                                compressed_len, output, logical_len);
    if (produced != logical_len ||
        vtx_crc32c(output, logical_len) !=
            vtx_cpu32(hdr->crc32c_uncompressed)) {
        vtx_buffer_free(output);
        return -EUCLEAN;
    }
    *output_out = output;
    *output_len_out = logical_len;
    return 0;
}

#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(vtx_compress_extent);
EXPORT_SYMBOL_GPL(vtx_decompress_extent);
EXPORT_SYMBOL_GPL(vtx_buffer_free);
#endif
