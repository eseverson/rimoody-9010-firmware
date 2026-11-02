CROSS   ?= $(HOME)/nds32/bin/nds32le-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
# same code model as the payload: absolute addresses, no gp-relative data (eCos owns $gp), no relaxation
CFLAGS  := -march=v3 -mcpu=n9 -mcmodel=large -mno-ext-perf2 -mno-fp-as-gp -Os -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-common -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra
LDFLAGS := -nostdlib -nostartfiles -Wl,--no-relax -Wl,--no-warn-rwx-segments

all: HOOK.BIN hook.dis LAUNCH.BIN launch.dis ENTRY.BIN entry.dis HOOKA.BIN STUBS.BIN stubs.sym

# audio run: the hook in AUDIO mode, and the call loggers linked over eCos's dead `pwm` handler (stubs.S)
hooka.elf: hook.c hook.ld
	$(CC) $(CFLAGS) -DAUDIO $(LDFLAGS) -Wl,-T,hook.ld -o $@ hook.c -lgcc

HOOKA.BIN: hooka.elf
	$(OBJCOPY) -O binary -R .bss $< $@

STUB_SITES := -Wl,--defsym=site_1=0x3bbf74 -Wl,--defsym=site_2=0x3bafe0 -Wl,--defsym=site_3=0x3baf48 \
              -Wl,--defsym=site_4=0x3aa8a0 -Wl,--defsym=site_5=0x3bb996 -Wl,--defsym=site_6=0x3aa50a \
              -Wl,--defsym=site_7=0x3aa184 -Wl,--defsym=site_8=0x3a3f8e -Wl,--defsym=site_9=0x3a4636 \
              -Wl,--defsym=site_16=0x5fd67a
stubs.elf: stubs.S Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Ttext=0x317cb6 -Wl,-e,common $(STUB_SITES) -o $@ stubs.S

STUBS.BIN: stubs.elf
	$(OBJCOPY) -O binary $< $@

stubs.sym: stubs.elf
	$(CROSS)nm $< | grep -E ' (stub_[0-9]+|common)$$' > $@
	$(OBJDUMP) -d $< > stubs.dis

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
	rm -f hooka.elf HOOKA.BIN stubs.elf STUBS.BIN stubs.sym stubs.dis hook.elf HOOK.BIN hook.dis hook.map launch.elf LAUNCH.BIN launch.dis launch.map entry.elf ENTRY.BIN entry.dis
