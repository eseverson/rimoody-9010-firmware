/* Hardware H.264 decoder bring-up (vdec.c), HU1 op 19. Net task only. */
#ifndef VDEC_H
#define VDEC_H
struct vdec_stats { unsigned int bitreads, decodes; };
extern struct vdec_stats vdec_stats;
typedef int (*vdec_fmt)(char *, const char *, ...);
int vdec_init(char *o, vdec_fmt fmt);
int vdec_bitread(unsigned int src, unsigned int len, char *o, vdec_fmt fmt);
int vdec_status(char *o, vdec_fmt fmt);
int vdec_decode(unsigned int src, unsigned int len, char *o, vdec_fmt fmt);   /* step 4: one I picture */
#endif
