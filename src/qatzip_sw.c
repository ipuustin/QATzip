/***************************************************************************
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2007-2022 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***************************************************************************/

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <zlib.h>
#include <pthread.h>

#include "cpa.h"
#include "cpa_dc.h"
#include "qatzip.h"
#include "qatzip_internal.h"
#include "qz_utils.h"

#define GZIP_WRAPPER 16

static const QzExtraField_T g_extra_field = {
    .st1 = 'Q',
    .st2 = 'Z',
    .x2_len = (uint16_t)sizeof(g_extra_field.qz_e),
    .qz_e.src_sz = 0,
    .qz_e.dest_sz = 0,
};

static void gen_qatzip_hdr(gz_header *hdr)
{
    qzMemSet(hdr, 0, sizeof(gz_header));
    hdr->extra = (Bytef *)&g_extra_field;
    hdr->extra_len = (uInt)sizeof(g_extra_field);
    hdr->os = 255;
}

/* The software failover function for compression request */
int qzSWCompress(QzSession_T *sess, const unsigned char *src,
                 unsigned int *src_len, unsigned char *dest,
                 unsigned int *dest_len, unsigned int last)

{
    int ret;
    z_stream *stream = NULL;
    int flush_flag;
    int last_loop_in;
    int last_loop_out;
    int current_loop_in;
    int current_loop_out;
    gz_header hdr;
    unsigned int left_input_sz = *src_len;
    unsigned int left_output_sz = *dest_len;
    unsigned int send_sz;
    unsigned int total_in = 0, total_out = 0;
    QzSess_T *qz_sess = NULL;
    int windows_bits = 0;
    int comp_level = Z_DEFAULT_COMPRESSION;
    QzDataFormat_T data_fmt = QZ_DATA_FORMAT_DEFAULT;
    unsigned int chunk_sz = QZ_HW_BUFF_SZ;
    QzGzH_T *qz_hdr = NULL;
    Qz4BH_T *qz4B_header = NULL;

    *src_len = 0;
    *dest_len = 0;

    /*check if setupSession called*/
    if (NULL == sess->internal) {
        ret = qzSetupSession(sess, NULL);
        if (QZ_SETUP_SESSION_FAIL(ret)) {
            return ret;
        }
    }

    qz_sess = (QzSess_T *) sess->internal;
    qz_sess->force_sw = 1;
    comp_level = (qz_sess->sess_params.comp_lvl == Z_BEST_COMPRESSION) ? \
                 Z_BEST_COMPRESSION : Z_DEFAULT_COMPRESSION;
    data_fmt = qz_sess->sess_params.data_fmt;
    chunk_sz = qz_sess->sess_params.hw_buff_sz;
    stream = qz_sess->deflate_strm;

    if (DeflateNull == qz_sess->deflate_stat) {
        if (NULL == stream) {
            stream = malloc(sizeof(z_stream));
            if (NULL == stream) {
                return QZ_FAIL;
            }

            qz_sess->deflate_strm = stream;
        }

        stream->zalloc = (alloc_func)0;
        stream->zfree = (free_func)0;
        stream->opaque = (voidpf)0;
        stream->total_in = 0;
        stream->total_out = 0;

        switch (data_fmt) {
        case QZ_DEFLATE_4B:
        case QZ_DEFLATE_RAW:
            windows_bits = -MAX_WBITS;
            break;
        case QZ_DEFLATE_GZIP:
        case QZ_DEFLATE_GZIP_EXT:
        default:
            windows_bits = MAX_WBITS + GZIP_WRAPPER;
            break;
        }

        /*Gzip header*/
        if (Z_OK != deflateInit2(stream,
                                 comp_level,
                                 Z_DEFLATED,
                                 windows_bits,
                                 MAX_MEM_LEVEL,
                                 Z_DEFAULT_STRATEGY)) {
            qz_sess->deflate_stat = DeflateNull;
            return QZ_FAIL;
        }
        qz_sess->deflate_stat = DeflateInited;

        if (QZ_DEFLATE_GZIP_EXT == data_fmt) {
            gen_qatzip_hdr(&hdr);
            if (Z_OK != deflateSetHeader(stream, &hdr)) {
                qz_sess->deflate_stat = DeflateNull;
                stream->total_in = 0;
                stream->total_out = 0;
                return QZ_FAIL;
            }
            qz_hdr = (QzGzH_T *)dest;
        } else if (QZ_DEFLATE_4B == data_fmt) {
            /* Need to reserve 4 bytes to fill the compressed length. */
            qz4B_header = (Qz4BH_T *)dest;
            dest = dest + sizeof(Qz4BH_T);
        }
    }

#ifdef QATZIP_DEBUG
    insertThread((unsigned int)pthread_self(), COMPRESSION, SW);
#endif

    do {
        send_sz = left_input_sz > chunk_sz ? chunk_sz : left_input_sz;
        left_input_sz -= send_sz;

        if (0 == left_input_sz && 1 == last) {
            flush_flag = Z_FINISH;
        } else {
            flush_flag = Z_FULL_FLUSH;
        }

        stream->next_in   = (z_const Bytef *)src + total_in;
        stream->avail_in  = send_sz;
        stream->next_out  = (Bytef *)dest + total_out;
        stream->avail_out = left_output_sz;

        last_loop_in = GET_LOWER_32BITS(stream->total_in);
        last_loop_out = GET_LOWER_32BITS(stream->total_out);


        ret = deflate(stream, flush_flag);
        if ((Z_STREAM_END != ret && Z_FINISH == flush_flag) ||
            (Z_OK != ret  && Z_FULL_FLUSH == flush_flag)) {
            QZ_ERROR("ERR: deflate failed with return code: %d flush_flag: %d\n", ret,
                     flush_flag);
            stream->total_in = 0;
            stream->total_out = 0;
            qz_sess->deflate_stat = DeflateNull;
            return QZ_FAIL;
        }

        current_loop_in = GET_LOWER_32BITS(stream->total_in) - last_loop_in;
        current_loop_out = GET_LOWER_32BITS(stream->total_out) - last_loop_out;
        left_output_sz -= current_loop_out;

        total_out += current_loop_out;
        total_in += current_loop_in;
        *src_len = total_in;
        *dest_len = total_out;

        if (NULL != qz_sess->crc32) {
            if (QZ_DEFLATE_RAW == data_fmt) {
                *qz_sess->crc32 = crc32(*qz_sess->crc32, src, *src_len);
            } else {
                if (0 == *qz_sess->crc32) {
                    *qz_sess->crc32 = stream->adler;
                } else {
                    *qz_sess->crc32 =
                        crc32_combine(*qz_sess->crc32, stream->adler, *src_len);
                }
            }
        }
    } while (left_input_sz);

    if (NULL != qz_sess->deflate_strm && 1 == last) {
        /*
         * When data_fmt is QZ_DEFLATE_GZIP_EXT,
         * we should fill src_sz & dest_sz in gzipext header field.
         */
        if (QZ_DEFLATE_GZIP_EXT == data_fmt && qz4B_header) {
            qz_hdr->extra.qz_e.src_sz  = stream->total_in;
            qz_hdr->extra.qz_e.dest_sz = stream->total_out -
                                         outputHeaderSz(data_fmt) - outputFooterSz(data_fmt);
        } else if (QZ_DEFLATE_4B == data_fmt && qz4B_header) {
            qz4B_header->blk_size = stream->total_out;
            *dest_len = *dest_len + sizeof(Qz4BH_T);
        }
        ret = deflateEnd(stream);
        stream->total_in = 0;
        stream->total_out = 0;
        qz_sess->deflate_stat = DeflateNull;
        if (Z_OK != ret) {
            return QZ_FAIL;
        }
    }

    return QZ_OK;
}

/* The software failover function for decompression request */
int qzSWDecompress(QzSession_T *sess, const unsigned char *src,
                   unsigned int *src_len, unsigned char *dest,
                   unsigned int *dest_len)
{
    z_stream *stream = NULL;
    int ret = QZ_OK;
    int zlib_ret = Z_OK;
    QzDataFormat_T data_fmt;
    int windows_bits = 0;
    unsigned int total_in;
    unsigned int total_out;
    unsigned int qz4B_header_len = 0;

    QzSess_T *qz_sess = (QzSess_T *) sess->internal;
    qz_sess->force_sw = 1;
    stream = qz_sess->inflate_strm;
    data_fmt = qz_sess->sess_params.data_fmt;

    if (NULL == stream) {
        stream = malloc(sizeof(z_stream));
        if (NULL == stream) {
            *src_len = 0;
            *dest_len = 0;
            return QZ_FAIL;
        }

        stream->zalloc = (alloc_func)0;
        stream->zfree  = (free_func)0;
        stream->opaque = (voidpf)0;
        stream->total_in    = 0;
        stream->total_out   = 0;
        qz_sess->inflate_strm = stream;
    }

    stream->next_in   = (z_const Bytef *)src;
    stream->avail_in  = *src_len;
    stream->next_out  = (Bytef *)dest;
    stream->avail_out = *dest_len;
    *src_len = 0;
    *dest_len = 0;

    QZ_DEBUG("decomp_sw data_fmt: %d\n", data_fmt);
    switch (data_fmt) {
    case QZ_DEFLATE_4B:
    case QZ_DEFLATE_RAW:
        windows_bits = -MAX_WBITS;
        break;
    case QZ_DEFLATE_GZIP:
    case QZ_DEFLATE_GZIP_EXT:
    default:
        windows_bits = MAX_WBITS + GZIP_WRAPPER;
        break;
    }

    if (InflateNull == qz_sess->inflate_stat) {
        if (QZ_DEFLATE_4B == data_fmt) {
            /* For QZ_DEFLATE_4B, we need to skip the header. */
            stream->next_in = (z_const Bytef *)stream->next_in + sizeof(Qz4BH_T);
            stream->avail_in = stream->avail_in - sizeof(Qz4BH_T);
            qz4B_header_len = sizeof(Qz4BH_T);
        }
        ret = inflateInit2(stream, windows_bits);
        if (Z_OK != ret) {
            ret = QZ_FAIL;
            goto done;
        }
        QZ_DEBUG("\n****** inflate init done with win_bits: %d *****\n", windows_bits);
        qz_sess->inflate_stat = InflateInited;
        stream->total_in = 0;
        total_in = 0;
        stream->total_out = 0;
        total_out = 0;
    } else {
        total_in = GET_LOWER_32BITS(stream->total_in);
        total_out = GET_LOWER_32BITS(stream->total_out);
    }

    zlib_ret = inflate(stream, Z_SYNC_FLUSH);
    switch (zlib_ret) {
    case Z_OK:
        if (QZ_LOW_DEST_MEM == sess->thd_sess_stat) {
            QZ_DEBUG("ERR: inflate failed with Z_DATA_ERROR\n");
            ret = QZ_DATA_ERROR;
            qz_sess->inflate_stat = InflateError;
            goto done;
        }
        ret = QZ_OK;
        qz_sess->inflate_stat = InflateOK;
        break;
    case Z_STREAM_END:
        ret = QZ_OK;
        qz_sess->inflate_stat = InflateEnd;
        break;
    case Z_DATA_ERROR:
        QZ_DEBUG("ERR: inflate failed with Z_DATA_ERROR\n");
        ret = QZ_DATA_ERROR;
        qz_sess->inflate_stat = InflateError;
        goto done;
    default:
        QZ_DEBUG("ERR: inflate failed with error code %d\n", ret);
        ret = QZ_FAIL;
        qz_sess->inflate_stat = InflateError;
        goto done;
    }

    *dest_len = GET_LOWER_32BITS(stream->total_out - total_out);
    /* for Deflate_4B, we need to add the length of Deflate 4B header. */
    *src_len = GET_LOWER_32BITS(stream->total_in - total_in + qz4B_header_len);

done:
    QZ_DEBUG("Exit qzSWDecompress total_in: %u total_out: %u "
             "avail_in: %u avail_out: %u msg: %s "
             "src_len: %u dest_len: %u\n",
             stream->total_in, stream->total_out,
             stream->avail_in, stream->avail_out,
             stream->msg,
             *src_len,
             *dest_len);
    if (zlib_ret == Z_STREAM_END || QZ_LOW_DEST_MEM == sess->thd_sess_stat) {
        if (Z_OK != inflateEnd(stream)) {
            QZ_DEBUG("inflateEnd failed.\n");
            ret = QZ_FAIL;
        }
        qz_sess->inflate_stat = InflateNull;
        QZ_DEBUG("\n****** inflate end done *****\n");
    }

    return ret;
}

int qzSWDecompressMultiGzip(QzSession_T *sess, const unsigned char *src,
                            unsigned int *src_len, unsigned char *dest,
                            unsigned int *dest_len)
{
    int ret = QZ_OK;
    unsigned int total_in = 0;
    unsigned int total_out = 0;
    const unsigned int input_len = *src_len;
    const unsigned int output_len = *dest_len;
    unsigned int cur_input_len = input_len;
    unsigned int cur_output_len = output_len;
#ifdef QATZIP_DEBUG
    insertThread((unsigned int)pthread_self(), DECOMPRESSION, SW);
#endif
    QZ_DEBUG("Start qzSWDecompressMultiGzip: src_len %u dest_len %u\n",
             *src_len, *dest_len);

    *src_len = 0;
    *dest_len = 0;

    while (total_in < input_len && total_out < output_len) {
        ret = qzSWDecompress(sess,
                             src + total_in,
                             &cur_input_len,
                             dest + total_out,
                             &cur_output_len);
        if (ret != QZ_OK) {
            goto out;
        }

        total_in  += cur_input_len;
        total_out += cur_output_len;
        cur_input_len  = input_len - total_in;
        cur_output_len = output_len - total_out;
        *src_len  = total_in;
        *dest_len = total_out;
    }

out:
    QZ_DEBUG("Exit qzSWDecompressMultiGzip: src_len %u dest_len %u\n",
             *src_len, *dest_len);
    return ret;
}
