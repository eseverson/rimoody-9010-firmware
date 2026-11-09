/* Physical DRAM map of the payload (notes/memory_map.md). The unit has 64 MB: CPU addresses 0x04000000..0x07FFFFFF are
 * aliases of 0x00000000..0x03FFFFFF (U-Boot runs from the alias, "relocated to 0x07CC3000" is 0x03CC3000), and 0x08000000
 * up hangs the bus. DMA masters (decoder, DDFCH, DSP) must be given addresses below MM_DRAM_END. Every fixed buffer lives
 * here; memmap_check.c asserts at compile time that the regions are in order, do not overlap and end below U-Boot. */
#ifndef MEMMAP_H
#define MEMMAP_H

#define MM_DRAM_END     0x04000000u   /* 64 MB */
#define MM_ALIAS_END    0x08000000u   /* 0x04000000..0x07FFFFFF read/write low memory again; above hangs the bus */
#define MM_PHYS(a)      ((unsigned int)(a) & (MM_DRAM_END - 1))

/* H.264 decoder (vdec.c) */
#define MM_VDEC_EVB       0x00000000u   /* bitstream ring, 2500 KB */
#define MM_VDEC_EVB_SZ    0x00271000u
#define MM_VDEC_SPMP      0x00400000u   /* 1028 KB */
#define MM_VDEC_SPMP_SZ   0x00101000u
#define MM_VDEC_SCS       0x00510000u   /* 2 KB; was 0x00500000, inside the SPMP ring's last 4 KB */
#define MM_VDEC_SCS_SZ    0x00000800u
#define MM_VDEC_WORK_3B   0x00540000u   /* 128 KB each (sizes not confirmed) */
#define MM_VDEC_WORK_3C   0x00580000u
#define MM_VDEC_WORK_SZ   0x00020000u
#define MM_VDEC_SLOTS     0x00600000u   /* 4 frame slots, 0x280000 apart: luma, CbCr +0x180000, MVC +0x240000 (0x3C000) */
#define MM_VDEC_SLOT_STEP 0x00280000u
#define MM_VDEC_SLOTS_SZ  (3 * MM_VDEC_SLOT_STEP + 0x240000u + 0x3C000u)

/* OSD video (osdvid.c): two 1024 x 600 RGB565 buffers */
#define MM_OSDVID_BUF0    0x01000000u
#define MM_OSDVID_BUF1    0x01200000u
#define MM_OSDVID_BUF_SZ  0x0012C000u

/* net.c scratch text */
#define MM_HELLO          0x01500000u   /* HELLO.TXT report, 0x800 */
#define MM_HELLO_SZ       0x00000800u
#define MM_MCU_RX         0x01600000u   /* MCU RX hex text */
#define MM_MCU_RX_SZ      0x00100000u

/* the payload: image + .bss (payload.ld asserts it ends below MM_LOG) */
#define MM_PAYLOAD        0x01800000u
#define MM_PAYLOAD_SZ     0x00200000u

#define MM_LOG            0x01A00000u   /* run log ring */
#define MM_LOG_SZ         0x000F0000u
#define MM_LOG_BOUNCE     0x01B00000u   /* fatwrite bounce buffer for the log */
#define MM_LOG_BOUNCE_SZ  0x00040000u
#define MM_PREV_LOG       0x01C00000u   /* tail of the previous run's log after a crash */
#define MM_PREV_LOG_SZ    0x00040000u
#define MM_BIGBSS         0x01D00000u   /* .bigbss (lwIP, AA), payload.ld asserts the end */
#define MM_BIGBSS_SZ      0x00200000u
#define MM_CRASH          0x01F00000u   /* crash record */
#define MM_CRUMB          0x01F0F000u   /* decoder breadcrumbs (vdec.c), survive a reset */
#define MM_VECTORS        0x01F10000u   /* exception vectors (IVB needs 64 KB alignment), 0x200 */
#define MM_CRASH_SZ       0x00010200u   /* crash record through the vectors */

#define MM_HU1_REPLY      0x02000000u   /* HU1 reply buffer, HU_MAX_REPLY + 256 */
#define MM_HU1_REPLY_SZ   0x00010100u
#define MM_WIFI_TXT       0x02100000u   /* WIFI.TXT, wiped after parsing */
#define MM_WIFI_TXT_SZ    0x00001000u
#define MM_RTOS_HEAP      0x02200000u   /* FreeRTOS heap_4 (configTOTAL_HEAP_SIZE), placed by payload.ld */
#define MM_RTOS_HEAP_SZ   0x00200000u
#define MM_DSP            0x02400000u   /* audio DSP window: microcode banks + data memory, as eCos */
#define MM_DSP_SZ         0x004C4800u
#define MM_LVGL_HEAP      0x02A00000u   /* LV_MEM_ADR / LV_MEM_SIZE (lv_conf.h) */
#define MM_LVGL_HEAP_SZ   0x00100000u
#define MM_LVGL_DRAW      0x02B00000u   /* LVGL partial draw buffer, 1024 x 120 x 4 */
#define MM_LVGL_DRAW_SZ   0x00078000u
#define MM_INSTALL        0x02B80000u   /* install to flash (install.c): the image being copied, then the plan's blocks */
#define MM_INSTALL_SZ     0x00380000u
#define MM_TRAMP          0x02F00000u   /* EXEC trampoline (tramp.ld), argv at +0xF000 */
#define MM_TRAMP_SZ       0x00010000u
#define MM_UPLOAD         0x03000000u   /* hu.py uploads (EXEC images up to 0x180000, tools/fb.py to 0x032F0080) */
#define MM_UPLOAD_SZ      0x00400000u
#define MM_FB             0x03400000u   /* console / UI framebuffer, 1024 x 600 x 4 */
#define MM_FB_SZ          0x00258000u
#define MM_FB_HDR         0x03660000u   /* OSD region header of the framebuffer (128 bytes) */
#define MM_OSDVID_HDR     0x03661000u   /* OSD region header of the video layer (128 bytes) */
#define MM_HDR_SZ         0x00002000u

/* U-Boot from here to the top: its stack grows down from gd->start_addr_sp (about 0x037B2F00), malloc area
 * 0x037B3000..0x03CC3000, code and data from 0x03CC3000. net.c checks the live values at boot. */
#define MM_UBOOT          0x03700000u
#define MM_UBOOT_STACK_MIN 0x00040000u  /* stack headroom the boot check wants between MM_UBOOT and the live sp */

#endif
