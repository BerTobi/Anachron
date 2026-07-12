/* png — minimal dependency-free PNG writer (truecolor 8-bit, no filtering,
 * stored-deflate). Made for screenshots: every byte format-valid, zero
 * compression libraries, works on anything with a C compiler — including
 * Windows XP. Stored deflate means the file is ~raw size; screenshots ride
 * to vision APIs as base64 where validity matters and size is bounded by
 * the capture-side downscale. */
#ifndef ANACHRON_PNG_H
#define ANACHRON_PNG_H

#include <stddef.h>

/* Write a WxH image of tightly-packed RGB triplets (row-major, top-down) to
 * `path` as a PNG. Returns 0 on success; -1 with a message in err. */
int png_write_rgb(const char *path, int w, int h, const unsigned char *rgb,
                  char *err, size_t errsz);

#endif /* ANACHRON_PNG_H */
