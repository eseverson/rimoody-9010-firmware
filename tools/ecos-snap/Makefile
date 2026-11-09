CROSS   ?= $(HOME)/nds32/bin/nds32le-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
# same code model as the payload: absolute addresses, no gp-relative data (eCos owns $gp), no relaxation
CFLAGS  := -march=v3 -mcpu=n9 -mcmodel=large -mno-ext-perf2 -mno-fp-as-gp -Os -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-common -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra
LDFLAGS := -nostdlib -nostartfiles -Wl,--no-relax -Wl,--no-warn-rwx-segments

all: HOOK.BIN hook.dis LAUNCH.BIN launch.dis ENTRY.BIN entry.dis HOOKA.BIN STUBS.BIN stubs.sym HOOKB.BIN STUBSB.BIN stubsb.sym HOOKE.BIN STUBSE.BIN stubse.sym

# audio run: the hook in AUDIO mode, and the call loggers linked over eCos's dead `pwm` handler (stubs.S)
hooka.elf: hook.c hook.ld
	$(CC) $(CFLAGS) -DAUDIO $(LDFLAGS) -Wl,-T,hook.ld -o $@ hook.c -lgcc

HOOKA.BIN: hooka.elf
	$(OBJCOPY) -O binary -R .bss $< $@

STUB_SITES := -Wl,--defsym=site_1=0x3bbf74 -Wl,--defsym=site_2=0x3bafe0 -Wl,--defsym=site_3=0x3baf48 \
              -Wl,--defsym=site_4=0x3aa8a0 -Wl,--defsym=site_5=0x3bb996 -Wl,--defsym=site_6=0x3aa50a \
              -Wl,--defsym=site_7=0x3aa184 -Wl,--defsym=site_8=0x3a3f8e -Wl,--defsym=site_9=0x3a4636 \
              -Wl,--defsym=site_16=0x5fd67a
BT_SITES := -Wl,--defsym=site_32=0x521aa8 -Wl,--defsym=site_33=0x3866ec -Wl,--defsym=site_34=0x385ea2 \
            -Wl,--defsym=site_35=0x385ef2 -Wl,--defsym=site_36=0x386394 -Wl,--defsym=site_16=0x5fd67a \
            -Wl,--defsym=site_17=0x602c62 -Wl,--defsym=site_18=0x600ea0
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
	rm -f hooke.elf HOOKE.BIN stubse.elf STUBSE.BIN stubse.sym stubse.dis hookb.elf HOOKB.BIN stubsb.elf STUBSB.BIN stubsb.sym stubsb.dis hooka.elf HOOKA.BIN stubs.elf STUBS.BIN stubs.sym stubs.dis hook.elf HOOK.BIN hook.dis hook.map launch.elf LAUNCH.BIN launch.dis launch.map entry.elf ENTRY.BIN entry.dis

# Bluetooth run: the hook in BT mode (no snapshots; resets 90 s after the RFCOMM connect ack AC 29, or at 10 min)
hookb.elf: hook.c hook.ld
	$(CC) $(CFLAGS) -DBT $(LDFLAGS) -Wl,-T,hook.ld -o $@ hook.c -lgcc

HOOKB.BIN: hookb.elf
	$(OBJCOPY) -O binary -R .bss $< $@

stubsb.elf: stubs.S Makefile
	$(CC) $(CFLAGS) -DBT $(LDFLAGS) -Wl,-Ttext=0x317cb6 -Wl,-e,common $(BT_SITES) -o $@ stubs.S

STUBSB.BIN: stubsb.elf
	$(OBJCOPY) -O binary $< $@

stubsb.sym: stubsb.elf
	$(CROSS)nm $< | grep -E ' (stub_[0-9]+|common|common_bytes)$$' > $@
	$(OBJDUMP) -d $< > stubsb.dis

# EQ run: the hook arms the loggers at 30 s and resets at 4 min; stubs for the DSP parameter setters
EQ_SITES := -Wl,--defsym=site_1=0x3bbf74 -Wl,--defsym=site_4=0x3aa8a0 -Wl,--defsym=site_10=0x3bd312 \
            -Wl,--defsym=site_11=0x3bd336 -Wl,--defsym=site_12=0x3bd2ca -Wl,--defsym=site_13=0x3bd2ee \
            -Wl,--defsym=site_14=0x3ab6b8 -Wl,--defsym=site_7=0x3aa184 -Wl,--defsym=site_16=0x5fd67a \
            -Wl,--defsym=site_17=0x602c62
hooke.elf: hook.c hook.ld
	$(CC) $(CFLAGS) -DEQ $(LDFLAGS) -Wl,-T,hook.ld -o $@ hook.c -lgcc

HOOKE.BIN: hooke.elf
	$(OBJCOPY) -O binary -R .bss $< $@

stubse.elf: stubs.S Makefile
	$(CC) $(CFLAGS) -DEQ $(LDFLAGS) -Wl,-Ttext=0x317cb6 -Wl,-e,common $(EQ_SITES) -o $@ stubs.S

STUBSE.BIN: stubse.elf
	$(OBJCOPY) -O binary $< $@

stubse.sym: stubse.elf
	$(CROSS)nm $< | grep -E ' (stub_[0-9]+|common)$$' > $@
	$(OBJDUMP) -d $< > stubse.dis
