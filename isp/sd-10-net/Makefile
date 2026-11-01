CROSS   ?= $(HOME)/nds32/bin/nds32le-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

# v3 ISA, soft-float ABI 2, no gp-relative data, absolute 32-bit calls, no linker relaxation (no ex9/ifc/gp).
# $r10 holds U-Boot's gd and must survive into every U-Boot call, so the compiler may not touch it.
CFLAGS  := -march=v3 -mcpu=n9 -mcmodel=large -mno-ext-perf2 -mno-fp-as-gp -Os -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-common -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fno-unwind-tables \
           '-ffixed-$$r10' -I. -Ifreertos/include -Ifreertos/port -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -nostdlib -nostartfiles -Wl,--no-relax -Wl,--no-warn-rwx-segments
LIBS    := -lgcc
BUILD_ID := $(shell date +%Y%m%d-%H%M%S)
RTOS    := freertos/tasks.c freertos/list.c freertos/queue.c freertos/timers.c freertos/heap_4.c freertos/port/port.c
SRCS    := head.S net.c crypto.c wpa.c ip.c mcu.c irq.c irq.S clk.c ehci.c usbh.c gt9.c console.c libc.c fw.S tramp_bin.S $(RTOS)
HDRS    := FreeRTOSConfig.h freertos/port/portmacro.h freertos/port/port_nds32.h crypto.h wpa.h ip.h mcu.h irq.h clk.h ehci.h usbh.h usbdev.h gt9.h console.h font8x16.h ub_sig.h rtl8188f_tables.h rtl8xxxu_regs.h

all: NET.BIN net.dis TRAMP.BIN tramp.dis

# the trampoline is linked for 0x02f00000 and embedded in NET.BIN (tramp_bin.S)
tramp.elf: tramp.c tramp.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,tramp.ld -Wl,-Map,tramp.map -o $@ tramp.c

TRAMP.BIN: tramp.elf
	$(OBJCOPY) -O binary $< $@

net.elf: $(SRCS) $(HDRS) rtl8188fufw.bin TRAMP.BIN payload.ld
	$(CC) $(CFLAGS) $(EXTRA) '-DBUILD_ID="$(BUILD_ID)"' $(LDFLAGS) -Wl,-T,payload.ld -Wl,-Map,net.map -o $@ $(SRCS) $(LIBS)

NET.BIN: net.elf
	$(OBJCOPY) -O binary -R .bss $< $@

%.dis: %.elf
	$(OBJDUMP) -d -r $< > $@

# host tests (plain gcc + python3 with the `cryptography` package)
HOSTCC ?= gcc
test: test/crypto_host test/wpa_host test/ip_host test/mcu_host test/con_host
	cd test && python3 -I test_crypto.py ./crypto_host && python3 -I test_wpa.py ./wpa_host && \
	  python3 -I test_ip.py ./ip_host && python3 -I test_mcu.py ./mcu_host && \
	  python3 -I test_console.py ./con_host

test/%_host: test/%_host.c crypto.c crypto.h wpa.c wpa.h ip.c ip.h mcu.c mcu.h console.c console.h font8x16.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

clean:
	rm -f net.elf NET.BIN net.dis net.map tramp.elf TRAMP.BIN tramp.dis tramp.map test/*_host
