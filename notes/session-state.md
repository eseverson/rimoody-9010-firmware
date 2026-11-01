# Session state (2026-10-31, autonomous touch work)

## Done
- Root cause of the "touch reboot" and of crash `vec7/itype f4 @ 21203c5e`: `vfmt` in net.c was unbounded; `ip.c:338` logs every HU1 command into the 512-byte `fmtbuf`, so a ~900-char command (tools/gt9.py packets) overwrote `reloc` (0x0181b61c) with ASCII ("000 " = 0x20303030) and the next U-Boot call (set_note -> get_timer) jumped to 0x21203c5e. Reproduced with RAM-only `mw.l` packets (scratchpad flood.py), no GPIO involved.
- First fix attempt (build 181317) hung the unit: `PUT(*s++)` inside a bounded macro never advanced `s` once full. Unit has been offline since ~18:15 and needs a power cycle (BOOT.BIN on the card is still the good 175013).
- Fixed build 20261031-182509 (isp/sd-10-net/NET.BIN): bounded vfmt (host-tested), 90 s hang watchdog in the tick ISR (pet in hb_tick + plat_cmd; crash brief `hang@pc`), HU1 op 14 TOUCH (gt9.c), gt9_release before exec. `make test` passes.
- tools/hu.py: `touch pins|probe|id|read REG LEN|poll|watch SECS|reset|release`.

## On hardware
- 182509: flood test clean, 15-min soak 54 rounds no crash. Touch GT911 at 0x5D works; front buttons are touch areas (notes/display_touch.md "Live touch").
- **Persisted 20261031-185340 to card BOOT.BIN (crc e744c8f3)** after a 6-min soak (59 rounds, no crash). It adds payload touch events: polls every 25 ms, logs `touch: down X Y [button N]` / `touch: up`, last event on the console's crash line. `hu.py touch events on|off`; `touch watch` turns events off first. Buttons verified 1..5 top to bottom.
- White haze after the hang + power cut: faded on its own; theory and checks in notes/display_touch.md. No screen blanking (owner wants nav always on).

## RTOS (owner decision 2026-10-31: FreeRTOS + own port)
- Plan and status: notes/rtos.md (authoritative). **Card BOOT.BIN = build 20261031-195947 (crc db3e0dfa)**: FreeRTOS + console task + clk.c + own EHCI driver by default; 6-min soak clean (30 rounds, 14000 transfers). Next: EHCI v2 (always-queued bulk IN), own `usb start`, SD/FAT.

## Next
0. RTOS milestone 2 (notes/rtos.md).
1. Use touch events for something: e.g. buttons -> MCU volume/mode frames (only known-safe frames), or a touch-driven console page.
2. If the haze recurs after a power cut: trace the stock shutdown (backlight `EE FF`, panel power) and replicate before deliberate resets.
3. Remaining backlog: 172901 heap-corruption crash (likely the vfmt overflow, now fixed), register walk paused at 0x9c000F00, audio DSP.
