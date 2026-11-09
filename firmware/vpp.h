/* VPP0 video layer bring-up (vpp.c), HU1 op 24. */
#ifndef VPP_H
#define VPP_H
typedef int (*vpp_fmt)(char *, const char *, ...);
/* p[0]: 0 status, 1 color bar [p[1] seconds, p[2] DMIX mode field, p[3] mode], 2 decoder picture, 3 restore, 4 poke addr val,
 * 5 clock, 6 linear NV12, 7 gated clock, 8 bisect mask, 9 probe mask [path] (notes/vpp0_display.md, round 2) */
int vpp_op(const unsigned char *p, int n, char *o, vpp_fmt fmt, unsigned int luma_idx, unsigned int chroma_idx);
void vpp_restore(void);
int vpp_active(void);
/* show a decoded picture (physical luma / chroma of a frame in the decoder's eCos layout, w x h) through VPP0 */
int vpp_show_frame(unsigned int luma, unsigned int chroma, unsigned int fw, unsigned int fh, unsigned int cx, unsigned int cy,
                   unsigned int cw, unsigned int ch);
int vpp_show_video(unsigned int luma, unsigned int chroma, unsigned int w, unsigned int h);
void vpp_hide(void);
#endif
