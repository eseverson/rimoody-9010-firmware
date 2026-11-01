/* Hardware H.264 decoder bring-up (vdec.c), HU1 op 19. Net task only. */
#ifndef VDEC_H
#define VDEC_H
struct vdec_stats { unsigned int bitreads, decodes; };
extern struct vdec_stats vdec_stats;
typedef int (*vdec_fmt)(char *, const char *, ...);
int vdec_init(char *o, vdec_fmt fmt);
int vdec_bitread(unsigned int src, unsigned int len, char *o, vdec_fmt fmt);
int vdec_status(char *o, vdec_fmt fmt);
int vdec_spmp_test(char *o, vdec_fmt fmt, unsigned int extras);
int vdec_crumb_report(char *o, vdec_fmt fmt);
void vdec_hw_reset(void);
int vdec_decode(unsigned int src, unsigned int len, char *o, vdec_fmt fmt, int kick, unsigned int skip,
                unsigned int maxpics);   /* I and P pictures; kick 0: prepare the first only; maxpics 0: all */
#endif
