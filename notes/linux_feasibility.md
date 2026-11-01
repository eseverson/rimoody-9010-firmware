# Linux on the SPHE8268K: feasibility versus bare metal

Status as of 2026-10-31. Desk research plus static analysis of the firmware we already dumped. No hardware access. Disassembly references point at `uboot-re/uboot.ann.dis` and `isp-re/ecos.ann.dis` in the earlier session's scratchpad (see memory `head-unit-context`). Register values come from the `sd-10-net` round log (`LOG00.TXT`, read by our payload under U-Boot). Related notes: [display_touch.md](display_touch.md), [audio.md](audio.md).

## Verdict

**Linux is not feasible on this unit in any practical sense. The CPU is configured with an Andes memory protection unit (MPU), not a page-based TLB MMU.** The only Linux port for this ISA (`arch/nds32`, removed from mainline after 5.17) requires an MMU (`config MMU def_bool y`, with no nommu option). Running Linux would mean writing nommu support for a dead architecture, plus a flat-binary userspace that no toolchain supports for nds32, plus drivers for display, audio, timer and USB. That is a multi-person-year project with no upstream to land in. The bare-metal path (our own payload on top of U-Boot services, LVGL on the OSD framebuffer) is the one that fits.

The decisive fact is `MMU_CFG.MMPS = 1`. The evidence and its limits are in the next section. One more hardware check would settle it beyond doubt (see "Confirming the MPU finding").

## 1. What the CPU registers say

All decodes use the field definitions in Linux 5.17 [`arch/nds32/include/asm/bitfield.h`](https://github.com/torvalds/linux/blob/v5.17/arch/nds32/include/asm/bitfield.h) and the arithmetic in [`arch/nds32/kernel/setup.c`](https://github.com/torvalds/linux/blob/v5.17/arch/nds32/kernel/setup.c) and [`arch/nds32/include/asm/nds32.h`](https://github.com/torvalds/linux/blob/v5.17/arch/nds32/include/asm/nds32.h).

Raw values from `sd-10-net` (taken under U-Boot): `PSW=0007400a IVB=00000801 CPU_VER=0d180037 ICM_CFG=000020c9 DCM_CFG=000020c9 MMU_CFG=4908bb05 MSC_CFG=1d284065 MMU_CTL=00072004 CACHE_CTL=00000033`.

| Register | Value | Decode |
|---|---|---|
| CPU_VER | 0x0D180037 | CPUID 0x0D. Linux prints this as "N13" (`cpu_series = 'N'` because the high nibble is 0, `cpu_id = 13`). REV 0x18. CFGID 0x0037: performance extension 1, 16-bit ISA, performance extension 2, string extension. **No FPU** (bit 3 clear). Bit 5 is set but Linux does not decode it. |
| ICM_CFG, DCM_CFG | 0x20C9 each | 128 sets × 2 ways × 32-byte lines = **8 KiB I-cache and 8 KiB D-cache**. Way size is 4 KiB, equal to the page size, so the VIPT alias problem would not arise at 4 KiB pages. No ILM/DLM banks (ILMB/DLMB = 0), no cache locking, BSAV = 1. |
| MSC_CFG | 0x1D284065 | BASEV = 2 (AndeStar V3 baseline), DIV and MAC present, EDM (debug) and PFM (perf counters) present, **no L2 cache** (L2C = 0), no audio/DSP ISA extension, no HSMP, no trace. |
| MMU_CFG | 0x4908BB05 | **MMPS = 1**, MMPV = 1, FATB = 0, TBW = 3, TBS = 7, EP8MIN4 = 1, EPSZ = 0x08, TLBLCK = 1, HPTWK = 0 (no hardware page-table walker), DE = 0 (little-endian), NTPT = 1, IVTB = 0, VLPT = 0, NTME = 1. |
| IVB | 0x00000801 | IVIC_VER = 1, NIVIC = 0 (the Linux `ativic32` driver maps this to 6 interrupt inputs), EVIC = 0 (internal vectored mode), IVBASE = 0. |
| PSW | 0x0007400A | INTL = 1, superuser, IT = DT = 0 (no address translation under U-Boot), WBNA = 1, CPL = 7, GIE = 0. |
| MMU_CTL | 0x00072004 | NTC0 = 2 (cacheability of non-translated partition 0), NTM1 = 1, NTM2 = 2, NTM3 = 3 (non-translated VA→PA partition mapping), D = 0, MPZIU = 0. |
| CACHE_CTL | 0x33 | I-cache and D-cache on, critical-word-forwarding and concurrent miss/write-back on. |

### Why MMPS = 1 means an MPU, not an MMU

- Andes' own startup code in U-Boot for the N1213 core treats only `MMPS == 2` as a TLB MMU: `andi $p0, $p0, 0x3 ! MMPS` / `li $p1, 0x2 ! TLB MMU` / `bne ... ; tlbop flushall` ([u-boot v2020.01 `arch/nds32/cpu/n1213/start.S`](https://github.com/u-boot/u-boot/blob/v2020.01/arch/nds32/cpu/n1213/start.S#L514-L518)). **Our U-Boot has the same check at `0xf001c8`, so on this board it skips the TLB flush** because MMPS reads 1.
- OpenOCD's Andes target (dropped in 0.12) reports address translation only when `memory_protection == 2`, and treats `memory_protection == 1` as an MPU ("Secure MPU has no IPC, IPSW, P_ITYPE"). It also exposes the page-table base register MR1 (`L1_PPTB`) only for value 2 ([openocd v0.11.0 `src/target/nds32.c`](https://github.com/openocd-org/openocd/blob/v0.11.0/src/target/nds32.c), `nds32_mmu()` and the register-enable logic near line 1255).
- Sunplus' eCos programs the protection unit as **8 fixed 512 MiB regions** (`ecos.ann.dis` at 0x4931c0–0x4932ee). It writes TLB_DATA and TLB_VPN and issues `tlbop targetwrite` with index 0x00000000, 0x20000000, …, 0xE0000000. The TLB_VPN values written are region limits (0x03FFFFFF, 0x01FFFFFF), not virtual page numbers. TLB_DATA carries a physical base plus attributes: DRAM is aliased at 0x00000000, 0x20000000 (uncached), 0x40000000 and 0x60000000, and 0xA0000000/0xC0000000/0xE0000000 are device or uncached. Nowhere does it build page tables.
- Andes sold the N13 with either an MMU or an MPU. A reseller page lists "MMU and MPU for Linux and RTOS" ([Fujisoft / IPROS AndesCore N13 listing](https://pr.mono.ipros.com/en/fsi/product/detail/2001521340)). An eCos-only DVD/car-AV SoC configured with the cheaper MPU is consistent with that.

Caveat: we have not read the AndeStar V3 SPA manual itself (it is not public). The MPU conclusion rests on Andes' and OpenOCD's code plus the eCos behavior. All three agree.

### What about the Linux strings in U-Boot?

U-Boot's `common/cmd_bootsp.c` contains `Run linux @ 0x%x`, a `res_linux_addr … res_iop_addr … mem=%dM@0x0 … vmalloc=320M` template, `[WDT] linux enable`, and `bootsp ecos|linux|all|isp|none`. This is Sunplus' shared U-Boot boot command. The same "IOP" (an 8051 I/O co-processor) and `res_*` reservations appear on Sunplus' Linux SoCs ([sunplus-plus1/iop](https://github.com/sunplus-plus1/iop) is the SP7021 IOP firmware). That code being compiled in says nothing about this core's MMU. The flash contains no Linux image, and the Linux path cannot boot a stock nds32 kernel on an MPU core.

### Confirming the MPU finding (optional, one SD round)

A payload under U-Boot can settle it for good. (a) Read `$L1_PPTB` (MR1) with an exception trap installed: on an MPU core this should take a reserved-register or instruction exception. (b) Write one 4 KiB TLB entry mapping VA 0x10000000 → PA of a known DRAM buffer using `tlbop RWR`, set PSW.DT, and read through the VA. On an MPU this cannot produce a 4 KiB remap.

## 2. Upstream Linux for NDS32 (status as of 2026)

- **Added in Linux 4.17 (2018)** via Greentime Hu's `nds32-for-linus-4.17` pull. It supported the N10, D10, N13, N15 and D15 cores and was tested on Andes' AE3XX FPGA platform ([pull request](https://lkml.rescloud.iu.edu/1804.0/00260.html), [Phoronix](https://www.phoronix.com/news/Linux-4.17-Andes-NDS32)). `arch/nds32/Kconfig.cpu` in 5.17 offers `CPU_N13` explicitly ([source](https://github.com/torvalds/linux/blob/v5.17/arch/nds32/Kconfig.cpu)).
- **Removed in Linux 5.18** by "nds32: Remove the architecture" (Alan Kao, March 2022, merged via Arnd Bergmann's asm-generic tree). The stated reasons: new Andes customers use RISC-V, and the remaining users are on Andes-provided longterm kernels ([commit](https://android-kvm.googlesource.com/linux/+/aec499c75cf8e0b599be4d559e6922b613085f8f), [5.18 asm-generic pull](https://lkml.iu.edu/2203.2/08394.html)). **Last mainline kernel with nds32: 5.17.**
- **MMU required:** `arch/nds32/Kconfig` has `config MMU def_bool y` and no nommu path. `head.S` unconditionally fills TLB entries from MMU_CFG (TBW/TBS/EPSZ) and enables IT/DT. The only interrupt controller and timer drivers are Andes' own `ativic32` and `atcpit100`. The only DTS is `ae3xx.dts` ([tree](https://github.com/torvalds/linux/tree/v5.17/arch/nds32)).
- **Toolchain:**
  - GCC: `gcc/config/nds32` is still in GCC trunk and was still getting commits in 2026 (e.g. "nds32: Fix rtx_costs return value", 2026-09-10). Our gcc 14 is fine for bare metal.
  - binutils: `gas/config/tc-nds32.c` is still on sourceware HEAD.
  - glibc: **never merged upstream**. The nds32 port reached v6 in 2019 and stopped ([v3 cover letter](https://sourceware.org/pipermail/libc-alpha/2018-June/094583.html)); `sysdeps/unix/sysv/linux/nds32` does not exist in glibc. Andes kept forks (`nds32-glibc-2.29-v6` and `ast-v3_2_x-release-v3` branches in [andestech/glibc](https://github.com/andestech/glibc)).
  - musl: no nds32 port ([arch list](https://git.musl-libc.org/cgit/musl/tree/arch)).
  - uClibc-ng: still has `libc/sysdeps/linux/nds32` ([wbx-github/uclibc-ng](https://github.com/wbx-github/uclibc-ng/tree/master/libc/sysdeps/linux/nds32)).
  - elf2flt (needed for nommu FLAT binaries): **no nds32 target** ([elf2flt.c](https://github.com/uclinux-dev/elf2flt/blob/main/elf2flt.c)).
- **Buildroot:** nds32 removed in **2022.08** (`BR2_nds32` "nds32 architecture removed" in [`Config.in.legacy`](https://github.com/buildroot/buildroot/blob/master/Config.in.legacy); [removal series](https://lore.kernel.org/all/20220607205921.3312256-1-thomas.petazzoni@bootlin.com/T/)). Last usable release: 2022.05.x.
- **OpenWrt:** never had an nds32 target. [andestech/openwrt](https://github.com/andestech/openwrt) is a RISC-V (AndeStar V5) fork.
- **Andes BSP:** [andestech/linux-deprecated](https://github.com/andestech/linux-deprecated) still has the pre-merge `nds32-4.14-rc8*` and `nds32-4.16-rc1-v7` branches. Every newer branch there and in [andestech/linux](https://github.com/andestech/linux) is RISC-V (AE350/AX45MP). [andestech/nds-toolchain](https://github.com/andestech/nds-toolchain) (last push 2022) and [andestech/build_script](https://github.com/andestech/build_script) (2018) are the V3-era leftovers. AndeSight IDE releases for V3 ("ast-v3_2_x") exist only as branch names. Nothing targets an MPU-configured core.

## 3. Sunplus: which IP blocks are shared, and what drivers exist

The SPHE8268K's peripheral map matches Sunplus' SP7021 ("Plus1"/"Pentagram", 4× Cortex-A7 + ARM926 + 8051 IOP). Same RGST base, same addresses, same register layouts:

| Block | SPHE8268K (ours) | SP7021 (upstream Linux) | Match evidence |
|---|---|---|---|
| Register bus | 0x9C000000, 32-word groups, masked writes | `soc@9c000000`, `ranges = <0 0x9c000000 0x400000>` ([sunplus-sp7021.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/sunplus/sunplus-sp7021.dtsi)) | identical base and grouping |
| UARTs | 0x9C000900 / 980 / 800 / 880 / 8780 (+8800) | `sunplus,sp7021-uart` at 0x900, 0x980, 0x800, 0x880, 0x8780; DATA at +0x00, LSR at +0x04 (bit0 TX not full, bit1 RX not empty) ([sunplus-uart.c](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/sunplus-uart.c), mainline since **5.18**) | same five addresses, same data/status offsets |
| Interrupt controller | eCos init at `0x608094` writes 7 words each at 0x9C000780 +0x00 (=~0), +0x1C (=0), +0x38 (=~0), +0x54 (=0); also touches 0x9C000A80; enables CPU INT_MASK bits 0–1 just before | `sunplus,sp7021-intc` at 0x9C000780 + 0x9C000A80: 224 IRQs = 7 words per bank; TYPE +0x00, POLARITY +0x1C, PRIORITY +0x38, MASK +0x54; two outputs EXT_INT0/1 to the parent ([irq-sp7021-intc.c](https://github.com/torvalds/linux/blob/master/drivers/irqchip/irq-sp7021-intc.c), mainline since **6.0**) | identical layout; on our chip the two outputs go to NDS32 IVIC inputs 0/1 |
| Watchdog | U-Boot `0xf00946` writes 0x3877, 0xAB00, CNT = 1, 0x4A4B to 0x9C000630/634 | `sunplus,sp7021-wdt` at 0x9C000630: STOP = 0x3877, UNLOCK = 0xAB00, RESUME = 0x4A4B ([sunplus_wdt.c](https://github.com/torvalds/linux/blob/master/drivers/watchdog/sunplus_wdt.c), [binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/watchdog/sunplus%2Csp7021-wdt.yaml), mainline since **5.19**) | identical address and magic values |
| SD card | U-Boot "SPSDC" driver (`spsdc_check_error`, "SPSDC [INFO] set clock to %d"); 0x9C003E80 referenced | Sunplus card controller family: upstream `sunplus,sp7021-mmc` ([sunplus-mmc.c](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sunplus-mmc.c), eMMC at 0x9C003B00, mainline since **6.2**); vendor SD drivers `sp_sd.c` / `sunplus_sd2.c` with `SPSDC_*` names ([sunplus-plus1/u-boot sp_sd.c](https://github.com/sunplus-plus1/u-boot/blob/master/drivers/mmc/sp_sd.c), [sunplus-plus1/linux sunplus_sd2.c](https://github.com/sunplus-plus1/linux/blob/master/drivers/mmc/host/sunplus_sd2.c)) | same driver family name and same media-type encoding (SD = 6, MS = 7); our SD base address not yet confirmed |
| Others upstream for SP7021 | — | clock (`clk-sp7021.c`), reset (`reset-sunplus.c`), pinctrl (`drivers/pinctrl/sunplus`), OCOTP, SPI, RTC, ethernet (`drivers/net/ethernet/sunplus`), USB2 PHY (`drivers/phy/sunplus`) | probably related; not compared |
| Not upstream for SP7021 | — | display/OSD/TCON, EHCI glue, I2C, audio, IOP | vendor tree only ([sunplus-plus1/linux](https://github.com/sunplus-plus1/linux)) |

So the IP reuse is real, and on an MMU core the UART, intc, watchdog and probably SD drivers would port with little change. No public Sunplus Linux BSP for SPHE8268, "gemini" or "8268k" turned up. Sunplus' public GitHub ([sunplus-plus1](https://github.com/sunplus-plus1)) covers only the ARM/RISC-V parts (SP7021, Q645, SP7350/Q654) and kernels 4.19–5.10 for those. This chip's eCos was built from `/home/jun.wan/workspace/ecos/project/8268/...` (string in `ecos.bin`), i.e. an internal Sunplus tree.

## 4. Effort: Linux versus bare metal

### Linux path (for completeness; blocked by the MPU)

Items 1–3 exist only because of the MPU. On an MMU core they would not be needed, and the remaining work would be roughly half of what is listed.

| # | Work item | Difficulty | Notes |
|---|---|---|---|
| 1 | **nommu support for arch/nds32** (entry/exit, signals, uaccess, mm, ptrace, no vDSO), on 5.17 or after forward-porting the deleted arch to a current kernel | Very high | No prior art for nds32. Other nommu ports (arm, riscv, xtensa) show the scale: thousands of lines and deep mm knowledge. |
| 2 | nommu userspace: FLAT or FDPIC binary format for nds32 (elf2flt target or FDPIC in gcc/binutils/kernel), uClibc-ng nommu build, BusyBox | Very high | elf2flt has no nds32 target; no FDPIC ABI for nds32 exists. |
| 3 | Unmaintained everything: kernel ≤ 5.17 or a self-maintained forward port, Buildroot ≤ 2022.05, uClibc-ng only, no glibc | Ongoing | Every later driver (e.g. RTL8188FU in rtl8xxxu, added in **6.2** ([commit](https://github.com/torvalds/linux/commit/c888183b21f36a247bb166ca9365705611bea847))) must be backported. |
| 4 | Board DTS, early boot from U-Boot (`bootm`/`go` with DTB), memory carve-outs for the IOP and the display | Medium | |
| 5 | Interrupts: ativic32 (6 inputs) as root, SP7021 intc chained on inputs 0/1 | Low–medium | Both drivers exist; the SP7021 driver needs a non-GIC parent. |
| 6 | Timer/clocksource: Sunplus STC or another timer (nds32 Linux only has `atcpit100`) | Medium | New driver; register map from eCos RE. |
| 7 | Console: no UART wired. Options: earlycon into a RAM ring buffer read back from SD; netconsole once Wi-Fi works; fbcon on the OSD framebuffer | Medium | Without a UART, early kernel bring-up is blind until the framebuffer or SD log works. |
| 8 | SD: adapt `sunplus-mmc` / vendor `sunplus_sd2` to our base and clocks | Medium | |
| 9 | USB EHCI ×2: `ehci-platform` + PHY init from U-Boot RE; RTL8188FTV via backported rtl8xxxu 8188f or the out-of-tree vendor driver; firmware blob | Medium–high | |
| 10 | Display: OSD/TCON framebuffer or DRM driver | High | No driver anywhere; register knowledge from our RE ([display_touch.md](display_touch.md)). A simple `simplefb` over U-Boot's already-running OSD0 layer is the cheap first step. |
| 11 | Touch: Goodix GT9xx (`goodix` driver exists) on `i2c-gpio` (GPIO91/92) | Low | |
| 12 | Audio: Sunplus DSP/DAC path as an ALSA driver | High | Undocumented; see [audio.md](audio.md). |
| 13 | MCU (AC6965A) protocol daemon in userspace over a UART | Low | |
| 14 | Media: no FPU (soft-float), 270 MHz, 8 KiB L1 caches, no L2. Software MP3/AAC decode is possible in fixed point; video decode would need Sunplus' hardware decoder, which is undocumented. | High | Applies to both paths. |

### Bare-metal path (what we have now)

| # | Work item | Difficulty | Notes |
|---|---|---|---|
| A | Runtime: payload loaded by U-Boot from SD; newlib + our gcc 14; reuse U-Boot services (SD/FAT, USB/Wi-Fi already working in `sd-10-net`) or reimplement them | Done / low | Already demonstrated. |
| B | Interrupts + tick: program the SP7021-style intc and an STC timer; vector table at IVB | Low–medium | Register layout confirmed above. |
| C | Display: own framebuffer on OSD0 (already shown in [display_touch.md](display_touch.md)) + LVGL flush callback | Low–medium | LVGL needs only a flush function and a tick. |
| D | Touch: GT9xx over bit-banged I2C | Low | The reboot during the live test needs explaining first. |
| E | MCU link: UART driver (SP7021 UART layout) + frame codec | Low | |
| F | Audio: DSP/DAC output for local media | High | Same unknowns as on Linux. |
| G | Networking: lwIP over the RTL8188 driver we have via U-Boot, or a port | Medium | |
| H | Optional RTOS: FreeRTOS on NDS32 V3 needs a small port (context switch, IVIC); eCos source for nds32 is Sunplus-internal | Medium | |
| I | Phone projection (CarPlay/Android Auto) | Very high on any path | Needs H.264 decode at the panel's 1024×600, TLS, USB gadget/host or Wi-Fi P2P, and for CarPlay an Apple MFi auth chip. 270 MHz with no FPU cannot decode H.264 in software at usable frame rates, so the Sunplus video decoder (undocumented) would be required. |

## 5. Recommendation and risks

For "own app in a car head unit (UI + media + phone projection later)": **bare metal, optionally with a small RTOS**, built on what already works (U-Boot as loader, SD payloads, Wi-Fi, OSD framebuffer). The reasons are facts above: the core has no page MMU, nds32 Linux needs one and is gone from mainline and Buildroot, the C library and binary-format story for nommu nds32 does not exist, and the Linux-only benefits (ready USB, Wi-Fi and network stacks) are mostly offset by having to backport drivers onto ≤ 5.17. The SP7021 driver sources remain useful as **register documentation** for the bare-metal drivers (intc, UART, watchdog, SD).

Main risks:

1. **Undocumented multimedia hardware.** Audio and especially video decode (needed for phone projection) rely on Sunplus DSP/decoder blocks that only the closed eCos drives. Without them, phone projection is out of reach on this SoC on any software path. That makes it the single biggest feasibility question, whether or not Linux was possible.
2. **No console and slow iteration.** Every experiment costs an SD swap and a reboot until a Wi-Fi or SD-log debug loop is built into the payload. The reboot during the touch experiment shows that faults are hard to see.
3. **Everything is reverse-engineered and single-sourced.** Each driver (timer, SD, USB/Wi-Fi, audio) comes from disassembly of the vendor U-Boot and eCos. The SP7021 upstream drivers help only where the IP really matches (UART, intc and watchdog confirmed; SD probable; display and audio unknown).
