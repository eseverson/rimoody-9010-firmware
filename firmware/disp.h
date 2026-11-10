/* Panel and display-pipeline bring-up for a boot that skipped U-Boot's logo routine (the SD-card installer's), see
 * disp.c and notes/display_touch.md ("Installer boot: no panel init"). */
#ifndef DISP_H
#define DISP_H

#define DISP_FLASH      0x98000000u   /* the NOR flash's read window (XIP), address = DISP_FLASH + flash offset */
#define DISP_FLASH_SIZE 0x01000000u
#define DISP_PART_PANEL 0x00018028u   /* flash partition table entry 1 {address, size}: the panel bin (entry 0: logo) */
#define DISP_WIDTH      1024          /* the only panel this firmware draws for */
#define DISP_HEIGHT     600

/* The panel bin's register list, checked: every entry's kind and target are known, the TGEN timing gives DISP_WIDTH x
 * DISP_HEIGHT, and the bin is the variant this unit's flash carries (one display, panel type 1). */
struct disp_panel {
    const unsigned char *ent;   /* n little-endian {address, value} pairs, inside the flash window */
    unsigned n;
    unsigned htotal, hact, vtotal, vend, vstart;   /* TGEN 0x9C006AA0..0xAB0 as the list sets them (& 0xFFF) */
};

/* Finds and checks the panel bin in a flash image (DISP_FLASH on the unit). 0 = ok; otherwise *why says what is wrong
 * and nothing may be written from it. */
int disp_panel_find(const unsigned char *flash, unsigned size, struct disp_panel *p, const char **why);

/* What the timing generator holds now. timing_ok: TGEN carries the panel's timing (U-Boot's init ran); moving: its line
 * counter advanced within ~200 us. Reads only. Returns timing_ok. */
struct disp_state {
    unsigned cfg, rst, htotal, hact, vtotal, vend, vstart, line0, line1, tcon;
    int timing_ok, moving;
};
int disp_state(const struct disp_panel *p, struct disp_state *s);

/* U-Boot's normal-boot display init, repeated without calling U-Boot (disp.c has the order and the sources). Writes
 * registers only; run it before the decoder, VPP0 or OSD0 are used. */
void disp_init(const struct disp_panel *p);

#endif
