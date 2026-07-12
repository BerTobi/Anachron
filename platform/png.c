/* png — see png.h. PNG is: 8-byte signature, then chunks of
 * [len be32][type 4][data][crc32(type+data)]. The pixel stream is one filter
 * byte (0 = None) per scanline followed by the RGB bytes, wrapped in a zlib
 * stream. We emit the zlib stream with STORED deflate blocks (BTYPE=00), which
 * is valid deflate that any decoder accepts and needs no compressor: just the
 * raw bytes in <=65535-byte blocks, plus an Adler-32 of the uncompressed data. */
#include "png.h"

#include <stdio.h>
#include <string.h>

/* CRC-32 (ISO 3309, the PNG polynomial), table built on first use. */
static unsigned long crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (unsigned long n = 0; n < 256; n++) {
        unsigned long c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static unsigned long crc_update(unsigned long c, const unsigned char *p, size_t n) {
    if (!crc_ready) crc_init();
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}

static void put_be32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

/* One whole chunk: length, type, data, crc. Returns 0 on write success. */
static int put_chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
    unsigned char hdr[8], crcb[4];
    put_be32(hdr, (unsigned long)n);
    memcpy(hdr + 4, type, 4);
    unsigned long crc = crc_update(0xffffffffUL, hdr + 4, 4);
    if (n) crc = crc_update(crc, data, n);
    put_be32(crcb, crc ^ 0xffffffffUL);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    if (fwrite(crcb, 1, 4, f) != 4) return -1;
    return 0;
}

/* Streaming writer for the IDAT payload: bytes fed to z_emit land inside
 * stored deflate blocks (opened on demand), with the chunk CRC and the
 * Adler-32 of the uncompressed stream maintained on the fly. */
struct zout {
    FILE *f;
    unsigned long crc, a1, a2;
    size_t raw_left;     /* uncompressed bytes still to come (sizes the blocks) */
    size_t block_left;   /* room left in the currently open stored block */
    int fail;
};

static void z_bytes(struct zout *z, const unsigned char *p, size_t n) {
    if (z->fail) return;
    if (fwrite(p, 1, n, z->f) != n) { z->fail = 1; return; }
    z->crc = crc_update(z->crc, p, n);
}

static void z_emit(struct zout *z, const unsigned char *p, size_t n) {
    while (n && !z->fail) {
        if (z->block_left == 0) {                 /* open the next stored block */
            size_t bn = z->raw_left > 65535 ? 65535 : z->raw_left;
            unsigned char bh[5];
            bh[0] = (bn == z->raw_left) ? 1 : 0;  /* BFINAL, BTYPE=00 */
            bh[1] = (unsigned char)(bn & 0xff); bh[2] = (unsigned char)(bn >> 8);
            bh[3] = (unsigned char)~bh[1];      bh[4] = (unsigned char)~bh[2];
            z_bytes(z, bh, 5);
            z->block_left = bn;
        }
        size_t take = n < z->block_left ? n : z->block_left;
        z_bytes(z, p, take);
        for (size_t i = 0; i < take; i++) {       /* Adler-32 over raw data */
            z->a1 = (z->a1 + p[i]) % 65521;
            z->a2 = (z->a2 + z->a1) % 65521;
        }
        p += take; n -= take;
        z->block_left -= take;
        z->raw_left -= take;
    }
}

int png_write_rgb(const char *path, int w, int h, const unsigned char *rgb,
                  char *err, size_t errsz) {
    if (w <= 0 || h <= 0 || !rgb) {
        snprintf(err, errsz, "png: empty image");
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errsz, "png: cannot open %s", path); return -1; }

    static const unsigned char sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    if (fwrite(sig, 1, 8, f) != 8) goto wfail;

    unsigned char ihdr[13];
    put_be32(ihdr, (unsigned long)w);
    put_be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 2;    /* color type: truecolor RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* deflate, filter 0, no interlace */
    if (put_chunk(f, "IHDR", ihdr, 13) != 0) goto wfail;

    /* IDAT: total zlib length is known up front (stored blocks add 5 bytes
     * each), so the chunk header goes first and the payload streams through. */
    size_t row = (size_t)w * 3;                   /* pixels per scanline */
    size_t raw = (row + 1) * (size_t)h;           /* + one filter byte per line */
    size_t nblocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + nblocks * 5 + 4;      /* zlib hdr + blocks + adler */

    unsigned char hdr[8];
    put_be32(hdr, (unsigned long)zlen);
    memcpy(hdr + 4, "IDAT", 4);
    if (fwrite(hdr, 1, 8, f) != 8) goto wfail;

    struct zout z;
    z.f = f;
    z.crc = crc_update(0xffffffffUL, hdr + 4, 4);
    z.a1 = 1; z.a2 = 0;
    z.raw_left = raw; z.block_left = 0; z.fail = 0;

    unsigned char zh[2] = {0x78, 0x01};           /* zlib: 32K window, fastest */
    z_bytes(&z, zh, 2);

    for (int y = 0; y < h; y++) {
        unsigned char filt = 0;                   /* filter: None */
        z_emit(&z, &filt, 1);
        z_emit(&z, rgb + (size_t)y * row, row);
    }

    unsigned char tail[8];
    put_be32(tail, (z.a2 << 16) | z.a1);          /* Adler-32 */
    z_bytes(&z, tail, 4);
    put_be32(tail + 4, z.crc ^ 0xffffffffUL);     /* IDAT chunk CRC */
    if (z.fail || fwrite(tail + 4, 1, 4, f) != 4) goto wfail;

    if (put_chunk(f, "IEND", NULL, 0) != 0) goto wfail;
    if (fclose(f) != 0) { snprintf(err, errsz, "png: close failed for %s", path); return -1; }
    return 0;

wfail:
    fclose(f);
    snprintf(err, errsz, "png: write failed for %s", path);
    return -1;
}
