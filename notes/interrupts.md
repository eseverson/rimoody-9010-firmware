# Interrupts, timer tick and exceptions on the SPHE8268K

Status as of 2026-10-31 (updated for build `20261031-173828`: tick on by default, non-blocking ISR heartbeat, crash record v2). Static analysis of the Sunplus eCos image and the stock U-Boot (disassembly references are to `isp-re/ecos.ann.dis` and `uboot-re/uboot.ann.dis` in the earlier session's scratchpad, see memory `head-unit-context`), the upstream SP7021 interrupt controller driver, and live tests on the unit with payload build `20261031-170042` (`firmware`, files `irq.S`, `irq.c`, `irq.h`). Related notes: [linux_feasibility.md](linux_feasibility.md).

## Summary

The CPU (AndesCore N13, AndeStar V3) runs its internal vectored interrupt controller (IVIC) with 6 inputs and 16-byte vector entries. An SP7021-style interrupt controller at 0x9C000780/0x9C000A80 collects 224 sources onto two outputs; EXT_INT0 is CPU input HW0 (vector 9). eCos's system clock is a timer block at 0x9C000640, interrupt 131, fed by 13.5 MHz. U-Boot never enables interrupts, never writes IVB or INT_MASK and never touches the interrupt controller or that timer, so the payload can own all of it while still calling U-Boot's polled services. Everything below marked "measured" was confirmed on the unit.

## CPU interrupt model

Under U-Boot the system registers read `IVB = 0x00000801` (IVBASE 0, ESZ 0, EVIC 0 = internal vectored, IVIC_VER 1, NIVIC 0 = 6 inputs, bit 0 set), `PSW = 0x0007400A` (GIE 0, INTL 1, superuser, WBNA, CPL 7) and `INT_MASK = 0xC0000000` (no HW inputs enabled).

eCos's HAL init (`0x606e7e`) sets `PSW = (PSW & 0xFFF83FE0) | 0x74008` (INTL 0, superuser, WBNA, CPL 7, GIE off) and `IVB = (IVB & 0x1FFF) | 0x00740000 | 0x4000` (vector base 0x00740000, ESZ = 16-byte entries). If IVB bit 0 is set it writes `INT_CTRL = 0`; if IVIC_VER is nonzero it writes `INT_MASK2 = 0` and `INT_PEND2 = ~0`. Then `0x606ee6` copies the vector table (link address 0x301040, 0x29C bytes) to 0x00740000 and fills a 40-entry handler table at 0x3012E0: entries 0–8 point to the exception path `0x301520`, entries 9–39 to the interrupt path `0x30156c`. Each 16-byte vector pushes the registers, loads its vector number into `$r0` and jumps to a common stub. The interrupt path saves `IPC`, `IPSW` and `$IFC_LP`, switches to an interrupt stack at 0x6DA2F0 and dispatches. The interrupt controller init `0x608076` clears IVB.EVIC and sets `INT_MASK |= 3` (HW0 and HW1).

Vector numbers (IVIC mode): 0 reset/NMI, 1–4 TLB-related (fill, PTE not present, misc, VLPT), 5 machine error, 6 debug, 7 general exception, 8 syscall, 9–14 hardware inputs HW0–HW5, 15 software interrupt. Measured: a `trap #0x7E57` arrives at vector 7 with `ITYPE = 0x7E5700E2`, i.e. SWID in bits 30:16, the vector number (7) in bits 9:5 and ETYPE 2 (trap) in bits 3:0; IPC points at the trap instruction itself, so resuming means IPC + 4. `EVA` equals IPC for a trap.

The exception system registers `EVA`, `P_EVA`, `ITYPE`, `P_ITYPE`, `MERR`, `IPSW`, `P_IPSW`, `IPC`, `P_IPC` and `OIPC` all exist: U-Boot's own exception dump at `0xf00280` reads them (U-Boot's vectors at 0xF00040 are never active because IVB.IVBASE stays 0).

## What U-Boot does with interrupts

Nothing after reset: the only GIE instruction in U-Boot is a `setgie.d` at `0xf001bc` in start-up, there are no writes to IVB, INT_MASK or PSW afterwards, no `iret`, and no accesses to 0x9C000780..0x9C0007EC or 0x9C000A80..0x9C000AFC. Its delay and `get_timer` use a different timer block at 0x9C003180 (`get_timer` 0xf00c2e, latch by writing 0x1234 to +0x68, read +0x60/+0x64). Its watchdog code (0xf00946, 0xf72b7e) uses only 0x9C000630/0x9C000634. Measured: with the payload's tick on at 100 Hz, IVB and PSW.GIE were never changed (the payload re-checks both every main-loop pass and counts fixes: 0) across `usb info`, `fatls mmc 0`, a blocking `sleep 7`, card writes of the log, and normal Wi-Fi traffic.

## Interrupt controller (0x9C000780 / 0x9C000A80)

The layout is Sunplus SP7021's, as in Linux `drivers/irqchip/irq-sp7021-intc.c` (mainline 6.0+): 224 sources in 7 words per bank. Group 0 at 0x9C000780: TYPE (+0x00, 1 = edge), POLARITY (+0x1C, 1 = low/falling), PRIORITY (+0x38, 1 = route to EXT_INT0), MASK (+0x54, 1 = enabled). Group 1 at 0x9C000A80: CLEAR (+0x00, write the bit), MASKED_EXT1 (+0x1C), MASKED_EXT0 (+0x38), GROUP summary (+0x7C, EXT1 pending groups in bits 6:0, EXT0 in bits 14:8).

eCos uses it exactly that way: init `0x608076` writes TYPE = ~0, POLARITY = 0, PRIORITY = ~0, MASK = 0; mask `0x608102` and unmask `0x60812a` clear or set the MASK bit; acknowledge `0x608150` writes the bit to CLEAR and executes `msync all`; configure `0x608176(irq, level, high)` sets TYPE (edge unless `level`) and POLARITY (active high/rising if `high`); the dispatcher `0x6080b6` scans MASKED_EXT1 and MASKED_EXT0 group by group for the lowest pending source.

Measured state under U-Boot: TYPE `00000000 103ffff3 00008000 000003fd 0000fff8 0000005f 00000000`, POLARITY all 0, PRIORITY all 1 (`...ffffffff 000000ff`), MASK all 0, CLEAR and masked status all 0. With the tick enabled every interrupt arrived on HW0 (vector 9), so EXT_INT0 is HW0. Which input EXT_INT1 drives (presumably HW1) is not proven.

## Timer for the tick (0x9C000640, IRQ 131)

eCos's `hal_clock_initialize` is `0x607f5a`, called from `0x64b3fc` with period 13500. It reads STC_CFG (0x9C00061C) and computes the timer input clock as `27 MHz / (2 * (STC_CFG[7:0] + 1))`, which is 13.5 MHz because STC_CFG is 0 (eCos writes it at `0x313b18`; it reads 0 under U-Boot too). It writes the timer at 0x9C000640: +0x00 control = 0, +0x04 prescale − 1, +0x08 reload (period − 1), +0x0C counter (period − 1), then control = 0xF. It configures interrupt 131 (0x83) as edge, rising (`0x608176(0x83, 0, 1)`). The clock ISR `0x64b428` calls an empty `hal_clock_reset` (`0x607fe2`) and acknowledges 131 in the controller; the timer reloads by itself. `hal_clock_read` (`0x607fea`) is `(period − 1) − (counter & 0xFFFF)`, so the counter counts down and is 16 bits wide.

Measured: the block reads all zero under U-Boot (stopped, unused). With prescale 100 (register 99) and reload 13500 (register 13499) the tick ran at 10.0 Hz; with reload 1350 it ran at 99.97 Hz against U-Boot's `get_timer` (1002 ticks in 10023 ms). That confirms the 13.5 MHz input. The STC divisor 0x9C00060C reads 0x8095 under U-Boot, the same value eCos programs (90 kHz STC). The STC watchdog registers at 0x9C000630/0x9C000634 were neither read nor written.

## Reset and the crash record

U-Boot's `reset_cpu` is `0xf0aa26`: `0x9C003104 |= 0x20`, `0x9C00317C = code` (0 = normal boot; "QART" requests the SD update path), `udelay(100)`, `0x9C000054 = 0x10001`. The payload's crash path does the same with code 0 and without calling U-Boot. Measured: after a deliberate fatal trap the unit was back on Wi-Fi (the card's BOOT.BIN) about 8 s later.

The crash record lives at 0x01F00000 (220 bytes: magic "HUCR", version, vector, ITYPE, IPC, IPSW, EVA, P_IPC, P_IPSW, P_ITYPE, P_EVA, interrupted `$sp`, PSW, tick count, INT_MASK, r0–r27, fp, gp, lp, build id, CRC-32), written back from the D-cache before the reset. Measured: the record survived the warm reset, xboot, U-Boot, `fatload` and a full run of the previous payload build (which does not know about it), and the next run of the interrupt build reported it with a good CRC and cleared it: `vec=7 itype=bad00e2 ipc=180abd6` (the `trap #0xbad` instruction in `irq_trap_fatal`), `r10=077b2f30` (U-Boot's gd), `gp=07d76cd8` (U-Boot's GOT base).

## What the payload implements

`irq_init()` runs at the start of `main`: it copies the 16-entry vector table (linked for 0x01F10000, stored in NET.BIN after .data) to 0x01F10000, writes back the D-cache and invalidates the I-cache for it, and then does what eCos's HAL does: INTL = 0, `IVB = (IVB & 0x1FFF) | 0x4000 | 0x01F10000` (measured `0x01F14801`), INT_CTRL = 0, INT_MASK2 = 0, INT_PEND2 = ~0. Interrupts stay off. Each vector pushes `$r0/$r1`, loads its number and jumps to `hu_trap_common`, which saves r2–r27, fp, gp, lp, IPC, IPSW and `$IFC_LP` on the interrupted stack, calls `hu_trap(vector, frame)` in C and restores everything before `iret`. The C code never uses `$r10` (`-ffixed-$r10`), `$gp` or the `$d0/$d1` accumulators, and calls nothing in U-Boot.

Vectors 9–15 go to the ISR. It only touches the payload's state, the timer's controller bit and UART1: acknowledge 131 (write CLEAR, `msync`), count the tick, send the MCU heartbeat every 3 s, and move received UART1 bytes into a 256-byte ring that the main loop's MCU parser reads. While interrupts are on, the main loop sends no heartbeat and does not read UART1 itself, and `uart_write` (MCU frames) runs with GIE off so frames never interleave with the ISR's heartbeat. A storm guard masks HW0/HW1 and stops the timer if the tick bit does not clear after 1000 consecutive acknowledgements, or if 1000 consecutive interrupts find nothing pending.

Since build `20261031-173828` the tick is turned on at 100 Hz as soon as `main` has installed the vectors and the console hook (`hu.py irq off` still works), after a 20-minute soak of build `170042` at 100 Hz was clean (120,763 ticks, 0 spurious, 0 ring drops, 402 ISR heartbeats). The ISR heartbeat no longer waits on UART1: a due frame is marked pending and its bytes go out only while the TX FIFO has room, continuing on later ticks; `uart_write` (GIE off) first finishes a heartbeat the ISR has started, so frames never interleave. The earlier version spun up to about 0.7 ms per heartbeat inside the ISR.

Vector 7 with SWID 0x7E57 (`irq_trap_test`) is counted and resumed at IPC + 4. Every other exception writes the crash record and resets. HU1 op 0x0B IRQ_CTL takes 0 = off, 1 = on with an optional u16 rate (3–1000 Hz, default 100), 2 = fatal trap after the reply, 3 = resumable trap, 4 = status. `hu.py irq status|on [HZ]|off|test|trap` drives it. PING adds `irq=`, `ticks=`, `irqs=` and `crash=` (the previous crash, once). Before EXEC and before `main` returns, `irq_shutdown()` turns everything off and restores IVB and PSW.INTL as found.

## Live test log (2026-10-31, build 20261031-170042, via `hu.py exec`)

1. EXEC from build 152706: the new build came up and installed its vectors (status: `ivb=1f14801(orig 801) psw=74008(orig 7400a) int_mask=c0000000`).
2. `irq test`: "handled and resumed", ITYPE 0x7E5700E2.
3. `irq on 10`: 50 ticks in about 5 s, all on HW0, `tmr=f/63/34bb`.
4. `irq on 100`: 1002 ticks in 10023 ms. `usb info` and `fatls mmc 0` ran normally with the tick on; during `sleep 7` the tick advanced 708 and the ISR sent 2 heartbeats; an MCU frame (`a1 10 1d`) went through the ISR ring into the parser; spurious 0, ring drops 0, IVB/GIE fixes 0.
5. `irq trap`: crash record written, unit reset and returned on the card's build within about 8 s; re-EXEC reported the record (see above).

## A real fault caught: U-Boot hush heap corruption (build 172901)

The first build with the tick on at boot crashed once, 6.3 s after enabling interrupts during its in-place restart (EXEC, `usb start`, Wi-Fi bring-up, card log writes): an alignment exception (ITYPE 0xE0, EVA 0x35) in U-Boot's `free()` (0xf1af72) called from hush `free_pipe` (0xf0c994) with an `argv` pointer of 0x39, so hush's heap data had been overwritten (the bytes look like ASCII digits). The crash path worked as designed: record, reset, and the next run reported it. The cause is not proven. The suspect is the ISR's old blocking heartbeat (up to 0.7 ms with interrupts off inside U-Boot's SD/USB drivers, which poll hardware FIFOs). After making the heartbeat non-blocking, 11 consecutive in-place restarts through the same path were clean. Crash records are now version 2 (magic "HUC2", so builds up to `170042` ignore them and leave them for the next new build) and add `note` (the U-Boot command being run through `ub_run`, set by `run`/`flush`/HU1 CMD), the uptime and the 16 words at the interrupted `$sp`. A record is reported in the log, in the first 3 PINGs and in the hello broadcasts for 2 minutes, then only on the console panel ("last crash").

## Second real fault: unaligned fatwrite source (build 173828)

A 15-minute stress soak crashed after about 9 minutes with an alignment exception at U-Boot link 0xf7707a, EVA 0x01A05D19, note `fatwrite $isp_if $isp_dev 1a05d19 LOG26.TXT 400`. U-Boot's SD driver (SPSDC) moves data by PIO with halfword accesses (`lhi` from the source buffer into the data register at +0xE4 when writing, `shi` when reading), so any buffer handed to fatwrite/fatload needs at least 2-byte alignment. The payload's log flush passed `LOG_BUF + (log_len - 0x40000)`, which is odd half the time once the log passes 256 KB. Fixed in build `20261031-175013`: the log window is copied into a 64-byte-aligned bounce buffer at 0x01B00000 first, and card writes now happen only for a crash report (one file, `C<crc>.TXT`) and for the MCU's shutdown request (`S<hex>.TXT`); routine LOGnn.TXT flushes and NET.TXT are gone. Every other buffer the payload hands to U-Boot (USB control/bulk buffers, the WIFI.TXT and HELLO areas) was already 64-byte aligned. This does not explain the build-172901 hush crash: that run's flushes all started at LOG_BUF + 0 (the log was still under 256 KB), so its cause stays open. The same 15-minute stress loop (`fatls mmc 0; fatload mmc 0 0x3800000 /BOOT.BIN; usb info` every ~2 s, `say` every 10 rounds, tick on at 100 Hz) then ran 392 rounds on build 175013 without a reset or crash.

## Open points and risks

- Only interrupt 131 is unmasked. Other sources (GPIO, UARTs, USB, display) and the EXT_INT1 → HW1 routing are untested.
- The ISR runs on whatever stack was interrupted (U-Boot's, about 0x077B2xxx). That is fine for this small ISR; an RTOS would want its own interrupt stack, as eCos uses at 0x6DA2F0.
- The hush heap corruption above may recur if its real cause is something else (for example a U-Boot DMA cache-maintenance problem that the ISR's timing only exposes). The v2 record's `note` will name the command if it does.
- An exception inside the ISR or inside the crash handler itself would be nested (INTL 2) and is not specially handled; it ends in the crash path again or hangs.
- The resumable and fatal traps prove the exception path, not the handling of real faults inside U-Boot code (bus errors, misaligned accesses). Those take the same path but have not been provoked.
- INT_CTRL, INT_MASK2 and INT_PEND2 are written as eCos writes them; their exact meaning on this core is not documented here.
