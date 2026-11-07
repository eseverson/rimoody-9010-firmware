CROSS   ?= $(HOME)/nds32/bin/nds32le-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
AR      := $(CROSS)ar

# v3 ISA, soft-float ABI 2, no gp-relative data, absolute 32-bit calls, no linker relaxation (no ex9/ifc/gp).
# $r10 holds U-Boot's gd and must survive into every U-Boot call, so the compiler may not touch it.
CFLAGS  := -march=v3 -mcpu=n9 -mcmodel=large -mno-ext-perf2 -mno-fp-as-gp -Os -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-common -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fno-unwind-tables \
           '-ffixed-$$r10' -I. -Ifreertos/include -Ifreertos/port -Ilwip -Ilwip/arch -Ilwip/src/include -Imbedtls/include '-DMBEDTLS_CONFIG_FILE="aa_tls_config.h"' -DLV_CONF_INCLUDE_SIMPLE -Ilvgl -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -nostdlib -nostartfiles -Wl,--no-relax -Wl,--no-warn-rwx-segments
LIBS    := -lgcc
BUILD_ID := $(shell date +%Y%m%d-%H%M%S)
RTOS    := freertos/tasks.c freertos/list.c freertos/queue.c freertos/timers.c freertos/heap_4.c freertos/port/port.c
MBEDTLS_SRC := $(wildcard mbedtls/library/*.c)
# mbedtls is built at -O2 into its own archive: the payload decrypts the whole AA video stream in software
MBEDTLS_OBJ := $(patsubst mbedtls/library/%.c,obj/mbedtls/%.o,$(MBEDTLS_SRC)) obj/gcm_alt.o
# LVGL v9 (lvgl/README.vendor, lv_conf.h): its own archive at -O2, like mbedtls
LVGL_SRC := $(shell find lvgl/src -name '*.c')
LVGL_OBJ := $(patsubst lvgl/%.c,obj/lvgl/%.o,$(LVGL_SRC))
LVGL_CFLAGS := -DLV_CONF_INCLUDE_SIMPLE -Ilvgl
LWIP_SRC := $(wildcard lwip/src/core/*.c lwip/src/core/ipv4/*.c) lwip/src/netif/ethernet.c
SRCS    := head.S memmap_check.c net.c crypto.c wpa.c ip.c mcu.c irq.c irq.S clk.c ehci.c usbh.c sd.c diskio.c fatfs/ff.c wpa_ap.c ap80211.c lwnet.c vdec.c h264p.c aa.c aa_tls.c aa_tcp.c aa_wl.c vpp.c osdvid.c audio.c ui.c ui_logo.c $(LWIP_SRC) gt9.c console.c libc.c fw.S tramp_bin.S $(RTOS)
HDRS    := memmap.h aa_wl.h FreeRTOSConfig.h freertos/port/portmacro.h freertos/port/port_nds32.h crypto.h wpa.h ip.h mcu.h irq.h clk.h ehci.h usbh.h wpa_ap.h ap80211.h lwnet.h vdec.h h264p.h aa.h aa_tls.h aa_tcp.h aa_tls_config.h vpp.h osdvid.h ui.h lv_conf.h lwip/lwipopts.h sd.h fatfs/ff.h fatfs/ffconf.h fatfs/diskio.h usbdev.h gt9.h console.h font8x16.h ub_sig.h rtl8188f_tables.h rtl8xxxu_regs.h

.PHONY: all test clean
all: NET.BIN net.dis TRAMP.BIN tramp.dis

# the trampoline is linked for 0x02f00000 and embedded in NET.BIN (tramp_bin.S)
tramp.elf: tramp.c tramp.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,tramp.ld -Wl,-Map,tramp.map -o $@ tramp.c

TRAMP.BIN: tramp.elf
	$(OBJCOPY) -O binary $< $@

obj/mbedtls/%.o: mbedtls/library/%.c aa_tls_config.h
	@mkdir -p obj/mbedtls
	$(CC) $(subst -Os,-O2,$(CFLAGS)) -c -o $@ $<

obj/gcm_alt.o: gcm_alt.c gcm_alt.h aa_tls_config.h
	@mkdir -p obj
	$(CC) $(subst -Os,-O2,$(CFLAGS)) -c -o $@ $<

mbedtls.a: $(MBEDTLS_OBJ)
	rm -f $@ && $(AR) rcs $@ $^

obj/lvgl/%.o: lvgl/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(subst -Os,-O2,$(CFLAGS)) $(LVGL_CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -c -o $@ $<

lvgl.a: $(LVGL_OBJ)
	rm -f $@ && $(AR) rcs $@ $^

net.elf: $(SRCS) $(HDRS) rtl8188fufw.bin TRAMP.BIN payload.ld mbedtls.a lvgl.a
	$(CC) $(CFLAGS) $(EXTRA) '-DBUILD_ID="$(BUILD_ID)"' $(LDFLAGS) -Wl,-T,payload.ld -Wl,-Map,net.map -o $@ $(SRCS) lvgl.a mbedtls.a $(LIBS)

NET.BIN: net.elf
	$(OBJCOPY) -O binary -R .bss $< $@

%.dis: %.elf
	$(OBJDUMP) -d -r $< > $@

# host tests (plain gcc + python3 with the `cryptography` package)
HOSTCC ?= gcc
test: test/crypto_host test/wpa_host test/ip_host test/mcu_host test/con_host test/h264p_host test/lwiptcp_host test/aawl_host test/wpaap_host test/dhcps_host test/ap80211_host test/osdvid_host test/gcm_host
	cd test && python3 -I test_crypto.py ./crypto_host && python3 -I test_wpa.py ./wpa_host && \
	  python3 -I test_ip.py ./ip_host && python3 -I test_mcu.py ./mcu_host && \
	  python3 -I test_console.py ./con_host && python3 -I test_h264p.py ./h264p_host && ./lwiptcp_host && ./aawl_host && ./wpaap_host && ./dhcps_host && python3 -I test_ap80211.py ./ap80211_host && python3 -I test_osdvid.py ./osdvid_host && python3 -I test_gcm.py ./gcm_host && ./test_tls.sh && ./test_aatcp.sh

test/%_host: test/%_host.c h264p.c h264p.h aa_wl.c aa_wl.h wpa_ap.c wpa_ap.h ap80211.c ap80211.h crypto.c crypto.h wpa.c wpa.h ip.c ip.h mcu.c mcu.h console.c console.h font8x16.h osdvid.c osdvid.h vdec.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

clean:
	rm -rf obj mbedtls.a lvgl.a && rm -f net.elf NET.BIN net.dis net.map tramp.elf TRAMP.BIN tramp.dis tramp.map test/*_host

test/gcm_host: test/gcm_host.c gcm_alt.c gcm_alt.h aa_tls_config.h
	$(HOSTCC) -O2 -Wall -Wextra -I. -Imbedtls/include '-DMBEDTLS_CONFIG_FILE="aa_tls_config.h"' -o $@ test/gcm_host.c gcm_alt.c mbedtls/library/aes.c mbedtls/library/platform_util.c mbedtls/library/platform.c
test/lwiptcp_host: test/lwiptcp.c lwip/lwipopts.h $(LWIP_SRC)
	$(HOSTCC) -O2 -w -Ilwip -Ilwip/arch -Ilwip/src/include -o $@ test/lwiptcp.c $(LWIP_SRC)
test/dhcps_host: test/dhcps.c dhcps.c dhcps.h lwip/lwipopts.h $(LWIP_SRC)
	$(HOSTCC) -O2 -w -I. -Ilwip -Ilwip/arch -Ilwip/src/include -o $@ test/dhcps.c dhcps.c $(LWIP_SRC)
