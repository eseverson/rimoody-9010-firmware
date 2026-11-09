/* Video through the OSD0 layer (osdvid.c). Net task only. */
#ifndef OSDVID_H
#define OSDVID_H
void osdvid_on(void);    /* OSD0 shows our YUY2 region (black) instead of the console */
void osdvid_off(void);   /* back to the console's region */
void osdvid_frame(unsigned int luma, unsigned int chroma, unsigned int w, unsigned int h);   /* convert a decoded picture and show it */
int osdvid_on_now(void);
unsigned int osdvid_frames(void);
unsigned int osdvid_last_us(void);
void osdvid_times(unsigned int *in, unsigned int *loop, unsigned int *out);   /* last conversion: cache in, pixels, cache out (us) */
extern int osdvid_fast;
/* the W x H window at (x0, y0) of a decoded picture in frame layout scan (vdec_scan_type) into linear YUY2 */
void osdvid_convert(unsigned char *dst, const unsigned char *luma, const unsigned char *chroma, unsigned int x0,
                    unsigned int y0, unsigned int w, unsigned int h, int scan);   /* 1: whole-cache maintenance instead of per-line */
#endif
