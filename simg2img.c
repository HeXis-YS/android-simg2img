/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE 1

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SPARSE_HEADER_MAJOR_VER 1
#define SPARSE_HEADER_MAGIC 0xED26FF3A
typedef struct {
    uint32_t magic;          /* 0xED26FF3A */
    uint16_t major_version;  /* (0x1) - reject images with higher major versions */
    uint16_t minor_version;  /* (0x0) - allow images with higer minor versions */
    uint16_t file_hdr_sz;    /* 28 bytes for first revision of the file format */
    uint16_t chunk_hdr_sz;   /* 12 bytes for first revision of the file format */
    uint32_t blk_sz;         /* block size in bytes, must be a multiple of 4 (4096) */
    uint32_t total_blks;     /* total blocks in the non-sparse output image */
    uint32_t total_chunks;   /* total chunks in the sparse input image */
    uint32_t image_checksum; /* CRC32 checksum of the original data, counting "don't care" */
    /* as 0. Standard 802.3 polynomial, use a Public Domain */
    /* table implementation */
} sparse_header_t;
#define SPARSE_HEADER_LEN (sizeof(sparse_header_t))

#define CHUNK_TYPE_RAW 0xCAC1
#define CHUNK_TYPE_FILL 0xCAC2
#define CHUNK_TYPE_DONT_CARE 0xCAC3
typedef struct {
    uint16_t chunk_type; /* 0xCAC1 -> raw; 0xCAC2 -> fill; 0xCAC3 -> don't care */
    uint16_t reserved1;
    uint32_t chunk_sz; /* in blocks in output image */
    uint32_t total_sz; /* in bytes of chunk input file including chunk header and data */
} chunk_header_t;
/* Following a Raw or Fill chunk is data.
 *  For a Raw chunk, it's the data in chunk_sz * blk_sz.
 *  For a Fill chunk, it's 4 bytes of the fill data.
 */
#define CHUNK_HEADER_LEN (sizeof(chunk_header_t))

#define BUF_SIZE (1024 * 1024)
uint8_t *copybuf;
uint8_t *fillbuf;

#define min(a, b) ((a) < (b) ? (a) : (b))

void usage() {
    fprintf(stderr, "Usage: simg2img [sparse_image_file] [raw_image_file]\n");
}

static ssize_t read_all(int fd, void *buf, size_t len) {
    size_t remaining = len;
    ssize_t ret;
    char *ptr = buf;

    while (remaining) {
        ret = read(fd, ptr, remaining);
        if (ret <= 0) {
            break;
        }
        ptr += ret;
        remaining -= ret;
    }

    if (ret < 0) {
        return ret;
    }

    return (ssize_t)(len - remaining);
}

static ssize_t write_all(int fd, const void *buf, size_t len) {
    size_t remaining = len;
    ssize_t ret;
    char *ptr = (char *)buf;

    while (remaining) {
        ret = write(fd, ptr, remaining);
        if (ret <= 0) {
            break;
        }
        ptr += ret;
        remaining -= ret;
    }

    if (ret < 0) {
        return ret;
    }

    return (ssize_t)(len - remaining);
}

static ssize_t skip_input(int fd, size_t len) {
    size_t remaining = len;
    ssize_t ret;

    if (lseek64(fd, len, SEEK_CUR) >= 0) {
        return (ssize_t)len;
    }

    while (remaining) {
        ret = read_all(fd, copybuf, min(remaining, BUF_SIZE));
        if (ret < 0) {
            perror("Could not seek or read to skip input data");
            exit(-1);
        }
        remaining -= ret;
    }

    return (ssize_t)len;
}

static int64_t skip_output(int fd, uint64_t len) {
    uint64_t remaining = len;
    ssize_t ret;

    if (lseek64(fd, (off_t)len, SEEK_CUR) >= 0) {
        return len;
    }

    if (*(uint32_t *)fillbuf != 0) {
        memset(fillbuf, 0, BUF_SIZE);
    }

    while (remaining) {
        ret = write_all(fd, fillbuf, (size_t)min(remaining, BUF_SIZE));
        if (ret < 0) {
            perror("Could not seek or write to skip output data");
            exit(-1);
        }
        remaining -= (uint64_t)ret;
    }

    return (int64_t)len;
}

int process_raw_chunk(int in, int out, uint32_t blocks, uint32_t blk_sz) {
    uint64_t len = (uint64_t)blocks * blk_sz;
    ssize_t ret;
    size_t chunk;

    while (len) {
        chunk = (size_t)min(len, BUF_SIZE);
        ret = read_all(in, copybuf, chunk);
        if (ret != (ssize_t)chunk) {
            fprintf(stderr, "read returned an error copying a raw chunk: %zd %zu\n",
                    ret, chunk);
            exit(-1);
        }
        ret = write_all(out, copybuf, chunk);
        if (ret != (ssize_t)chunk) {
            fprintf(stderr, "write returned an error copying a raw chunk\n");
            exit(-1);
        }
        len -= chunk;
    }

    return blocks;
}

uint32_t process_fill_chunk(int in, int out, uint32_t blocks, uint32_t blk_sz) {
    uint64_t len = (uint64_t)blocks * blk_sz;
    ssize_t ret;
    size_t chunk;
    uint32_t fill_val;
    uint32_t *fillbuf32;

    /* Fill fillbuf with the fill value */
    ret = read_all(in, &fill_val, sizeof(fill_val));
    fillbuf32 = (uint32_t *)fillbuf;
    if (*fillbuf32 != fill_val) {
        for (int i = 0; i < (BUF_SIZE / sizeof(fill_val)); i++) {
            fillbuf32[i] = fill_val;
        }
    }

    while (len) {
        chunk = min(len, BUF_SIZE);
        ret = write_all(out, fillbuf, chunk);
        if (ret != (ssize_t)chunk) {
            fprintf(stderr, "write returned an error copying a raw chunk\n");
            exit(-1);
        }
        len -= chunk;
    }

    return blocks;
}

int process_skip_chunk(int out, uint32_t blocks, uint32_t blk_sz) {
    /* len needs to be 64 bits, as the sparse file specifies the skip amount
     * as a 32 bit value of blocks.
     */
    uint64_t len = (uint64_t)blocks * blk_sz;

    skip_output(out, len);

    return blocks;
}

int main(int argc, char *argv[]) {
    int in;
    int out;
    unsigned int i;
    sparse_header_t sparse_header;
    chunk_header_t chunk_header;
    uint32_t total_blocks = 0;
    int ret;

    if (argc > 3 || (argc > 1 && strcmp(argv[1], "--help") == 0)) {
        usage();
        exit(-1);
    }

    if (argc < 2 || strcmp(argv[1], "-") == 0) {
        in = STDIN_FILENO;
    } else {
        in = open(argv[1], O_RDONLY);
        if (in < 0) {
            fprintf(stderr, "Cannot open input file %s\n", argv[1]);
            exit(-1);
        }
    }

    if (argc < 3 || strcmp(argv[2], "-") == 0) {
        out = STDOUT_FILENO;
    } else {
        out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            fprintf(stderr, "Cannot open output file %s\n", argv[2]);
            exit(-1);
        }
    }

    ret = read_all(in, &sparse_header, sizeof(sparse_header));
    if (ret != sizeof(sparse_header)) {
        fprintf(stderr, "Error reading sparse file header\n");
        exit(-1);
    }

    if (sparse_header.magic != SPARSE_HEADER_MAGIC) {
        fprintf(stderr, "Bad magic\n");
        exit(-1);
    }

    if (sparse_header.major_version != SPARSE_HEADER_MAJOR_VER) {
        fprintf(stderr, "Unknown major version number\n");
        exit(-1);
    }

    copybuf = malloc(BUF_SIZE);
    if (copybuf == NULL) {
        fprintf(stderr, "Cannot malloc copy buf\n");
        exit(-1);
    }

    fillbuf = malloc(BUF_SIZE);
    if (fillbuf == NULL) {
        fprintf(stderr, "Cannot malloc fill buf\n");
        exit(-1);
    }
    memset(fillbuf, 0, BUF_SIZE);

    if (sparse_header.file_hdr_sz > SPARSE_HEADER_LEN) {
        /* Skip the remaining bytes in a header that is longer than
         * we expected.
         */
        skip_input(in, sparse_header.file_hdr_sz - SPARSE_HEADER_LEN);
    }

    for (i = 0; i < sparse_header.total_chunks; i++) {
        ret = read_all(in, &chunk_header, sizeof(chunk_header));
        if (ret != sizeof(chunk_header)) {
            fprintf(stderr, "Error reading chunk header\n");
            exit(-1);
        }

        if (sparse_header.chunk_hdr_sz > CHUNK_HEADER_LEN) {
            /* Skip the remaining bytes in a header that is longer than
             * we expected.
             */
            skip_input(in, sparse_header.chunk_hdr_sz - CHUNK_HEADER_LEN);
        }

        switch (chunk_header.chunk_type) {
        case CHUNK_TYPE_RAW:
            if (chunk_header.total_sz != (sparse_header.chunk_hdr_sz +
                                          (chunk_header.chunk_sz * sparse_header.blk_sz))) {
                fprintf(stderr, "Bogus chunk size for chunk %d, type Raw\n", i);
                exit(-1);
            }
            total_blocks += process_raw_chunk(in, out,
                                              chunk_header.chunk_sz, sparse_header.blk_sz);
            break;
        case CHUNK_TYPE_FILL:
            if (chunk_header.total_sz != (sparse_header.chunk_hdr_sz + sizeof(uint32_t))) {
                fprintf(stderr, "Bogus chunk size for chunk %d, type Fill\n", i);
                exit(-1);
            }
            total_blocks += process_fill_chunk(in, out,
                                               chunk_header.chunk_sz, sparse_header.blk_sz);
            break;
        case CHUNK_TYPE_DONT_CARE:
            if (chunk_header.total_sz != sparse_header.chunk_hdr_sz) {
                fprintf(stderr, "Bogus chunk size for chunk %d, type Dont Care\n", i);
                exit(-1);
            }
            total_blocks += process_skip_chunk(out,
                                               chunk_header.chunk_sz, sparse_header.blk_sz);
            break;
        default:
            fprintf(stderr, "Unknown chunk type 0x%4.4x\n", chunk_header.chunk_type);
        }
    }

    /* If the last chunk was a skip, then the code just did a seek, but
     * no write, and the file won't actually be the correct size.  This
     * will make the file the correct size.  Make sure the offset is
     * computed in 64 bits, and the function called can handle 64 bits.
     */
    if (ftruncate64(out, (uint64_t)total_blocks * sparse_header.blk_sz)) {
        fprintf(stderr, "Error calling ftruncate() to set the image size\n");
        exit(-1);
    }

    close(in);
    close(out);

    if (sparse_header.total_blks != total_blocks) {
        fprintf(stderr, "Wrote %d blocks, expected to write %d blocks\n",
                total_blocks, sparse_header.total_blks);
        exit(-1);
    }

    exit(0);
}
