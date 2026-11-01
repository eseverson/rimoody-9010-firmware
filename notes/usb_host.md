# USB host bring-up (for our own `usb start`)

Source: vendor U-Boot 2014.04 disassembly (`uboot-re/uboot.ann.dis` in the first session's scratchpad, see memory head-unit-context). Mapped by a Haiku pass on 2026-10-31; the uphy_init register sequence below was checked against the disassembly by hand (0xf7f5c2..0xf7f81a). Everything else is from that pass and still unverified.

## Call chain
- `usb start|reset` handler 0xf160f4 -> usb_init-like 0xf17874 (clears 32 usb_dev entries of 0x9C0 bytes at link 0xFC5840, loops over 2 controllers, "scanning bus %d") -> EHCI lowlevel init 0xf7ecfe(index) -> Sunplus bring-up 0xf7f8f2 -> `uphy_init` 0xf7f5c2 and `usb_power_init` 0xf7f86a.
- Controller table at link 0xFA1B38: [0] 0x9C102100 (EHCI0), [1] 0x9C104100 (EHCI1, Wi-Fi). Ctrl structs at 0x123B540 + idx * 0x140.
- Delay helper 0xf86a40(n) spins on the counter at 0x9E839408 (unit unsure; probably ms).
- Enumeration (usb_new_device, port reset) not located yet; 0xf17178 is the candidate.

## uphy_init (0xf7f5c2), both PHYs
1. 0x9C00005C = 0x03000300, delay 1; = 0x03000000, delay 1 (looks like a masked-write reset register: high half = mask)
2. 0x9C004AF0 = 0x87474002; 0x9C004B70 = 0x87474004
3. 0x9C004AF8 and 0x9C004B78: 0x88, 0x80, 0x88, 0x80 (delay 1 after each pair), then 0x00, delay 20
4. 0x9C00005C = 0x00C000C0, delay 1; = 0x00C00000, delay 1
5. 0x9C004ACC |= 0xF; 0x9C004B4C |= 0xF
6. OTP 0x9C00AF18: bits[4:0] -> uphy0 disconnect level into 0x9C004A9C bits[4:0]; bits[12:8] -> uphy1 into 0x9C004B1C bits[4:0]. Mapping per the pass: c in 1..30 ? (c < 12 ? c + 2 : c) : 13. Printed as "uphy0_disc / uphy1_disc".
7. OTP 0x9C00AF08: bit0 -> 0x9C004AF4 |= 0x40 ("uphy0 rx clk inv"); bit1 -> 0x9C004B74 |= 0x40.

## usb_power_init (0xf7f86a)
- 0x9C00018C = 0x70007 for host (0x40004 then 0x30000 for device).

## EHCI init after HCRESET (0xf7ecfe)
- 4 KB periodic frame list, every entry T (phys | 2), PERIODICLISTBASE = it.
- USBCMD = (v & ~0xB2) | 1 (run), poll bit 0, up to 50 retries.
- CONFIGFLAG |= 1, delay 5.

## Plan
Our own `usb start` = uphy_init + usb_power_init + HCRESET + the init above + root-port reset (PORTSC 0x9C104164: PR for 50 ms, wait PE) + enumeration on EHCI1 at address 0 (GET_DESCRIPTOR 8 / 18, SET_ADDRESS, GET_CONFIGURATION descriptor, SET_CONFIGURATION), filling a struct with U-Boot's struct usb_device layout (usbdev.h) so net.c keeps using `wdev` unchanged. Test first with U-Boot's `usb start` still run before it (re-enumerate only), then without it.
