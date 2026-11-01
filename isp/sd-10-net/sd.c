/* Our own SD card block access (notes/rtos.md, SD/FAT milestone), phase 1: the card as U-Boot left it (initialized,
 * selected, 4-bit, ~45 MHz), single-block CMD17 / CMD24 by PIO, the way U-Boot's SPSDC driver does it.
 *
 * Controller: 0x9C003E80 (U-Boot's host->base, registered at 0xf77194; send_cmd 0xf76c60, command issue 0xf76a5e).
 * Register use, all from that disassembly (offsets from the base):
 *   0xEC command index | 0x40, 0xF0..0xFC argument bytes MSB..LSB, 0xB0 bit 0 cleared before a command
 *   0xB8 config: bit 0 PIO, bit 2 single block, bits 5:4 transfer (1 write, 2 read), bits 8:6 response handling
 *   0xC4 bit 13 long (136-bit) response; also the clock divider [11:0] and bus width (bit 12 = 4-bit), left alone
 *   0xB4 blocks - 1, 0xD4 block size - 1, 0x204 = 0x12 read / 0x21 write
 *   0xC8 bit 0 starts the command
 *   0xCC status: bit 1 response ready, bit 2 TX FIFO empty, bit 3 RX FIFO full, bit 5 DAT0 high (not busy),
 *        bit 6 response timeout, bits 7/8 data timeouts, bit 9 CRC7, bit 10 CRC token, bit 11 CRC16
 *   0xD0 state: bit 14 finished, bit 13 error
 *   0xE4 / 0xE8 PIO data out / in, one halfword per access
 *   0x100 / 0x104 response bytes (48-bit: 0x100 = index and bits 39:16, 0x104 low halfword = bits 15:0)
 *   error recovery: 0xC0 = 7, 0x20C = 6 and wait bit 2 clear, 0x210 = 0, 1, 0
 * The status bits match the SP7021 SD driver (drivers/mmc/host/sunplus-mmc.c) shifted by 0x2C.
 *
 * Net task only (one SD user at a time). Each block command runs with the scheduler suspended (interrupts stay on):
 * a block takes well under a millisecond, and a task switch in the middle would otherwise count against the
 * timeouts below and reset the controller while the card is still sending (suspected cause of the first boot-time test's failure, 2026-10-31).
 */
#include "sd.h"
#include "clk.h"
#include "FreeRTOS.h"
#include "task.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define SDB 0x9c003e80u
#define R(o) (*(volatile u32 *)(SDB + (o)))
#define RB(o) (*(volatile u8 *)(SDB + (o)))

struct sd_stats sd_stats;
static int addr_shift = -1;   /* argument = lba << addr_shift: 0 block-addressed (SDHC), 9 byte-addressed; -1 unknown */

static void sw_reset(void)
{
    R(0xc0) = 7;
    R(0x20c) = 6;
    u32 t0 = clk_ms();
    while ((R(0x20c) & 4) && clk_ms() - t0 < 50)
        ;
    R(0x210) = 0;
    R(0x210) = 1;
    R(0x210) = 0;
    sd_stats.resets++;
}

static int wait_bits(u32 off, u32 mask, u32 ms)   /* 0 when any bit of mask is set; -1 on D0 error or timeout */
{
    u32 t0 = clk_ms();
    for (;;) {
        int late = clk_ms() - t0 > ms;   /* sampled first, so the condition is always checked once after it */
        if (R(off) & mask)
            return 0;
        if (R(0xd0) & 0x2000)
            return -1;
        if (late)
            return -2;
    }
}

/* One command with an R1 response and an optional single 512-byte block. Returns 0, or <0 with sd_stats.last_cc. */
static int cmd(u32 idx, u32 arg, u8 *rd, const u8 *wr, u32 *r1)
{
    RB(0xec) = (u8)(idx | 0x40);
    RB(0xf0) = (u8)(arg >> 24);
    RB(0xf4) = (u8)(arg >> 16);
    RB(0xf8) = (u8)(arg >> 8);
    RB(0xfc) = (u8)arg;
    R(0xb0) = R(0xb0) & ~1u;
    u32 cfg = (R(0xb8) & ~0x1f0u) | 0x1c0;   /* R1: response with CRC */
    R(0xc4) = R(0xc4) & ~0x2000u;
    if (rd || wr) {
        R(0xb4) = 0;
        R(0xd4) = 511;
        if (rd) {
            cfg = (cfg & ~0xf0u) | 0x20;
            R(0x204) = 0x12;
        } else {
            cfg = (cfg & ~0x30u) | 0x10;
            R(0x204) = 0x21;
        }
        cfg |= 4 | 1;   /* single block, PIO */
    }
    R(0xb8) = cfg;
    R(0xc8) = R(0xc8) | 1;

    int e = 0;
    if (rd) {
        for (u32 i = 0; i < 512 && !e; i += 2) {
            if ((e = wait_bits(0xcc, 8, 250)))
                break;
            u16 h = (u16)R(0xe8);
            rd[i] = (u8)h;
            rd[i + 1] = (u8)(h >> 8);
        }
    } else if (wr) {
        for (u32 i = 0; i < 512 && !e; i += 2) {
            if ((e = wait_bits(0xcc, 4, 250)))
                break;
            R(0xe4) = (u32)(wr[i] | wr[i + 1] << 8);
        }
    }
    if (!e && !(e = wait_bits(0xcc, 2, 250))) {
        u32 a = R(0x100), b = R(0x104) & 0xffff;
        *r1 = a << 8 | b >> 8;
    }
    if (!e)
        e = wait_bits(0xd0, 0x4000, 500);
    if (!e && (R(0xd0) & 0x2000))
        e = -1;
    if (e) {
        sd_stats.last_cc = R(0xcc);
        sd_stats.last_d0 = R(0xd0);
        sd_stats.errors++;
        sw_reset();
        return e;
    }
    if (*r1 & 0xfdf98008u) {   /* R1 error bits (out of range, address, block length, erase, WP, CC, ECC, ...) */
        sd_stats.last_r1 = *r1;
        sd_stats.errors++;
        return -3;
    }
    return 0;
}

static int busy_wait(void)   /* after a write: the card holds DAT0 low while programming */
{
    u32 t0 = clk_ms();
    while (!(R(0xcc) & 0x20))
        if (clk_ms() - t0 > 1000)
            return -1;
    return 0;
}

static int rd1(u32 arg, u8 *buf)
{
    u32 r1;
    for (int t = 0; t < 3; t++) {
        vTaskSuspendAll();
        int e = cmd(17, arg, buf, 0, &r1);
        xTaskResumeAll();
        if (!e)
            return 0;
    }
    return -1;
}

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* Block (SDHC) or byte addressing: read the MBR, then the first partition's boot sector both ways. */
int sd_init(void)
{
    static u8 b[512] __attribute__((aligned(4)));
    if (R(0x00) != 6 && (R(0x00) & 7) != 6)
        return -1;
    if (rd1(0, b) || b[510] != 0x55 || b[511] != 0xaa)
        return -2;
    u32 lba = le32(b + 0x1c6);
    if (b[0x1c2] == 0 || lba == 0)
        lba = 0;   /* no partition table: a FAT volume from sector 0 */
    sd_stats.part_lba = lba;
    for (int s = 0; s <= 9; s += 9) {
        if (!rd1(lba << s, b) && b[510] == 0x55 && b[511] == 0xaa && b[11] == 0 && b[12] == 2) {   /* 512 bytes/sector */
            addr_shift = s;
            sd_stats.addr_shift = (u32)s;
            return 0;
        }
        if (!lba)
            break;
    }
    return -3;
}

int sd_read(u32 lba, u8 *buf, u32 n)
{
    if (addr_shift < 0)
        return -1;
    for (u32 i = 0; i < n; i++, lba++, buf += 512) {
        if (rd1(lba << addr_shift, buf))
            return -1;
        sd_stats.reads++;
    }
    return 0;
}

int sd_write(u32 lba, const u8 *buf, u32 n)
{
    if (addr_shift < 0)
        return -1;
    for (u32 i = 0; i < n; i++, lba++, buf += 512) {
        u32 r1;
        int ok = 0;
        for (int t = 0; t < 3 && !ok; t++) {
            if (busy_wait())
                return -2;
            vTaskSuspendAll();
            ok = !cmd(24, lba << addr_shift, 0, buf, &r1);
            xTaskResumeAll();
        }
        if (!ok || busy_wait())
            return -1;
        sd_stats.writes++;
    }
    return 0;
}
