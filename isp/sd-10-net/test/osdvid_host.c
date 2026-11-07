/* Host driver for osdvid.c's de-tiler (test_osdvid.py): osdvid_host LUMA CHROMA X0 Y0 W H SCAN OUT converts the window
 * of the tiled planes (the decoder's pitch, 1920) into linear YUY2. */
#define OSDVID_HOST
#include "../osdvid.c"
#include <stdio.h>
#include <stdlib.h>
int vdec_scan_type;
static unsigned char *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        exit(2);
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    unsigned char *d = malloc(*n);
    if (fread(d, 1, *n, f) != (size_t)*n)
        exit(2);
    fclose(f);
    return d;
}
int main(int argc, char **argv)
{
    if (argc != 9)
        return 2;
    long nl, nc;
    unsigned char *l = load(argv[1], &nl), *c = load(argv[2], &nc);
    u32 x0 = atoi(argv[3]), y0 = atoi(argv[4]), w = atoi(argv[5]), h = atoi(argv[6]);
    unsigned char *o = malloc(w * h * 2);
    osdvid_convert(o, l, c, x0, y0, w, h, atoi(argv[7]));
    FILE *f = fopen(argv[8], "wb");
    fwrite(o, 1, w * h * 2, f);
    fclose(f);
    return 0;
}
