/* Video through the OSD0 layer (osdvid.c). Net task only. */
#ifndef OSDVID_H
#define OSDVID_H
void osdvid_on(void);    /* OSD0 shows our YUY2 region (black) instead of the console */
void osdvid_off(void);   /* back to the console's region */
void osdvid_frame(unsigned int luma, unsigned int chroma);   /* convert a decoded picture and show it */
int osdvid_on_now(void);
unsigned int osdvid_frames(void);
unsigned int osdvid_last_us(void);
#endif
