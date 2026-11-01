CROSS   ?= $(HOME)/nds32/bin/nds32le-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

# v3 ISA, soft-float ABI 2, no gp-relative data, absolute 32-bit calls, no linker relaxation (no ex9/ifc/gp).
# $r10 holds U-Boot's gd and must survive into every U-Boot call, so the compiler may not touch it.
CFLAGS  := -march=v3 -mcpu=n9 -mcmodel=large -mno-ext-perf2 -mno-fp-as-gp -Os -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-common -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fno-unwind-tables \
           '-ffixed-$$r10' -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -nostdlib -nostartfiles -Wl,--no-relax -Wl,--no-warn-rwx-segments -Wl,-T,payload.ld -Wl,-Map,cap.map
LIBS    := -lgcc

all: CAP.BIN cap.dis

cap.elf: cap.c ub_sig.h payload.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ cap.c $(LIBS)

CAP.BIN: cap.elf
	$(OBJCOPY) -O binary -R .bss $< $@

cap.dis: cap.elf
	$(OBJDUMP) -d -r $< > $@

clean:
	rm -f cap.elf CAP.BIN cap.dis cap.map
