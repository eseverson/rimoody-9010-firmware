/* Our own SD card block access (sd.c). Net task only. */
#ifndef SD_H
#define SD_H
struct sd_stats { unsigned int reads, writes, errors, resets, last_cc, last_d0, last_r1, part_lba, addr_shift; };
extern struct sd_stats sd_stats;
int sd_init(void);   /* 0, or <0: no card / no MBR / no FAT boot sector found either way */
int sd_read(unsigned int lba, unsigned char *buf, unsigned int n);          /* 512-byte sectors; 0 or -1 */
int sd_write(unsigned int lba, const unsigned char *buf, unsigned int n);   /* 0 or <0 */
#endif
