CROSS   ?= $(HOME)/nds32/bin/nds32le-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
# same code model as the payload: absolute addresses, no gp-relative data (eCos owns $gp), no relaxation
CFLAGS  := -march=v3 -mcpu=n9 -mcmodel=large -mno-ext-perf2 -mno-fp-as-gp -Os -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-common -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra
LDFLAGS := -nostdlib -nostartfiles -Wl,--no-relax -Wl,--no-warn-rwx-segments

all: HOOK.BIN hook.dis LAUNCH.BIN launch.dis ENTRY.BIN entry.dis

hook.elf: hook.c hook.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,hook.ld -Wl,-Map,hook.map -o $@ hook.c -lgcc

HOOK.BIN: hook.elf
	$(OBJCOPY) -O binary -R .bss $< $@

hook.dis: hook.elf
	$(OBJDUMP) -d $< > $@

launch.elf: launch_head.S launch.c launch.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,launch.ld -Wl,-Map,launch.map -o $@ launch_head.S launch.c -lgcc

LAUNCH.BIN: launch.elf
	$(OBJCOPY) -O binary -R .bss $< $@

launch.dis: launch.elf
	$(OBJDUMP) -d $< > $@

entry.elf: entry.S entry.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,entry.ld -o $@ entry.S

ENTRY.BIN: entry.elf
	$(OBJCOPY) -O binary $< $@

entry.dis: entry.elf
	$(OBJDUMP) -d $< > $@

clean:
	rm -f hook.elf HOOK.BIN hook.dis hook.map launch.elf LAUNCH.BIN launch.dis launch.map entry.elf ENTRY.BIN entry.dis
