/* VPP0 video layer bring-up (vpp.c), HU1 op 24. */
#ifndef VPP_H
#define VPP_H
typedef int (*vpp_fmt)(char *, const char *, ...);
/* p[0]: 0 status, 1 color bar [p[1] seconds, p[2] DMIX mode field, p[3] mode], 2 decoder picture, 3 restore, 4 poke addr val,
 * 5 clock, 6 linear NV12, 7 gated clock, 8 bisect mask, 9 probe mask [path] (notes/vpp0_display.md, round 2) */
int vpp_op(const unsigned char *p, int n, char *o, vpp_fmt fmt, unsigned int luma_idx, unsigned int chroma_idx);
void vpp_restore(void);
int vpp_active(void);
#endif
