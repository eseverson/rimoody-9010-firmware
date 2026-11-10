# Install to flash: boot path and a payload copy in NOR

Status 2026-11-09: written and host-tested (`install.c`, `test/install_host.c`, `test/test_install.py`), build 20261109-143336. Nothing has run on the unit yet. The NOR driver it builds on (`nor.c`) is proven on the unit: erase, program and verify through the controller and the XIP window, protection cleared and restored, 0x05 status polls held up to 34 ms (notes/nor_flash.md).

Two stages, both started from Settings > System or `hu.py nor install`:

- **Stage 1, "Boot from the SD card, else from flash"**: the NOR boot script at flash 0xE00000 becomes version 2, and on a fresh unit U-Boot's default environment gets `bootcmd=source 98e00000;bootsp` with xboot's word sums recomputed (what `tools/patch_uboot.py` + `tools/flash_bootcmd.py` did from the PC). On a unit that already has the patched environment only the script block is rewritten.
- **Stage 2, "Install this firmware to flash"**: a copy of the payload with a header in the free region 0xC70000..0xDFFFFF. It needs stage 1: only the version 2 script ever loads it.

## Boot order (script version 2)

1. The SD card's `/BOOT.BIN` (mmc 0, then mmc 1), exactly as the version 1 script: the same pinmux, UART1 and MCU frame lines, `wdt stop all`, `go 0x1800000 hello`, `go 0x1800000 main`. The only additions on this path are `setenv hu_src sd` and clearing `hu_ok` / `hu_src` / `hu_nor` at the start.
2. Else the NOR copy, only if all of these hold: the header word at 0xC70000 is "HUNC"; the CRC-32 of 0xC70100..0xDFFFFF (the whole region past the header: image, then 0xFF) equals the header's span CRC; the copy in RAM after `cp.l` to 0x1800000 has the same CRC. Then `setenv hu_src nor` and the same `go` sequence.
3. Else the script ends and bootcmd's `;bootsp` starts stock eCos. A missing or damaged script (`source` checks its header and data CRCs) also ends in `bootsp`.

The script reads and copies the whole region (0x18FF00 bytes) rather than a length taken from flash: no `setexpr` is needed, and a header cannot steer the copy. `crc32 addr len dst` stores its result as a native word (U-Boot 0xF0C870: `swi` at 0xF0C906; this U-Boot has no `crc32 -v`, its `crc32` takes at most 4 arguments), and `cmp.l` compares it with the header word. The scratch words at 0x033F0000 are zeroed first, so a store that did not happen cannot match (the installer refuses a span CRC of 0). There is no `itest` in this U-Boot; `test`, `cmp`, `cp`, `crc32`, `mw`, `setenv`, `true` / `false` and `echo` are all in its command table.

Which path was taken is left in `hu_src`: `hu.py cmd 'printenv hu_src'`. The payload does not ask at boot (running U-Boot commands at boot cost AA its first picture). The version 1 script leaves it unset.

The script text lives in `install.c` (`script_text`); `hu.py nor install script` prints it from the running payload.

## Flash layout

| Flash | What | Written by |
|---|---|---|
| 0x8A0000..0x8AFFFF | block holding U-Boot's uImage header (at 0x8A40E0): header sum at +4, data sum at +24 | stage 1, fresh unit only |
| 0x920000..0x92FFFF | block holding U-Boot's default environment (at 0x92F09B in the original dump) | stage 1, fresh unit only |
| 0xC70000..0xC700FF | stage 2 header | stage 2 |
| 0xC70100..0xDFFFFF | stage 2 image, then 0xFF | stage 2 |
| 0xE00000..0xE0FFFF | NOR boot script (legacy uImage, type script) | stage 1 |

The environment and header blocks are not fixed addresses in the code: they are found in the live U-Boot image each time (see "Version tolerance").

Largest image: 0x18FF00 bytes (1,637,120). NET.BIN is 0x1230D0 (1,192,144) at this build. Larger images are refused.

### Stage 2 header (little-endian words)

| Offset | Field |
|---|---|
| 0x00 | magic "HUNC" (0x434E5548), programmed last; 0xFFFFFFFF while being written, 0 while being removed |
| 0x04 | header version, 1 |
| 0x08 | image length |
| 0x0C | CRC-32 of the image |
| 0x10 | span CRC: CRC-32 of 0xC70100..0xDFFFFF (image, then 0xFF); what the boot script checks |
| 0x14 | install counter: 1 on a blank region, the previous copy's + 1 otherwise |
| 0x18 | header length, 0x100 |
| 0x20 | build id from the image's HUNE header (+0x48), NUL-terminated, 56 bytes |
| 0xFC | CRC-32 of bytes 0x04..0xFB: identifies the header as ours with or without the magic |

## Checks before any byte is written

Everything is read fresh through the flash controller (fast read 0x0B). Planning (`inst_plan_boot`, `inst_region`, `inst_plan_copy`) is pure code that the host test runs against dumps.

Stage 1 (refusal reason in quotes, as `install status` prints it):

- JEDEC ID 0x204018 (XM25QH128C), checked by the driver before every erase or program ("unexpected JEDEC ID").
- The U-Boot uImage at 0x8A40E0 passes xboot's check as it is now: magic, size 0x1000..0x1FFFC0, the 0x5A verify flag set (stricter than xboot, which skips the sums without it), header word sum, body word sum ("no U-Boot uImage at the expected address", "U-Boot image size out of range", "U-Boot header has no xboot verify flag", "U-Boot header word sum does not match", "U-Boot body word sum does not match").
- The 44-byte default environment occurs exactly once in the U-Boot body, counting both the stock form (`bootcmd=bootsp`, `bootdelay=0`, `baudrate=921600`) and the patched form ("U-Boot default environment not found", "... found more than once"). It must lie inside xboot's largest U-Boot extent and not in the body's last partial word.
- The script block is blank, holds our script (a valid script uImage named `nor boot script` or `nor boot script vN`, nothing but 0xFF after it), or is an interrupted stage 1 write of the same new script (page 0 blank, every other page blank or equal to the new one). Anything else: "the boot script block holds something that is not ours". A version above 2: "a newer boot script is installed; this build would downgrade it".
- Each new block is built from a fresh read of the live block with only the intended bytes changed: the 44 environment bytes and the two sum words. The diff against the live block is checked with the same rule `nor_boot_write` enforces.
- The planned result is read back through an overlay (flash with the new blocks in place) and must pass the same xboot check, with the patched environment found exactly once at the same place ("internal: the patched U-Boot would fail the xboot check").

Stage 2:

- Stage 1 is installed and checks out: patched environment, our script version 2 or later, the U-Boot checks above ("stage 1 is not installed: the boot script would never load the copy").
- The image: 0x100..0x18FF00 bytes, HUNE header version 1 at +0x40 with a printable build id. From `hu.py` it is uploaded and its CRC checked again by the payload; from the UI it is the card's `BOOT.BIN`, which must be the running payload: same length and the same bytes as RAM except `.data` ("BOOT.BIN is not the running build"). The bytes written are the file's, not RAM's: `.data` changes once the payload runs, and the copy must start like a fresh load.
- The region (all 25 blocks) is blank, or holds our own copy: a header whose header CRC is valid (magic present, or still blank after an interrupted install), and nothing but 0xFF from the end of its image to 0xDFFFFF ("the copy region holds data that is not ours"). Only blocks that hold data are erased.

## Undo

Each stage can be removed again (Settings > Flash > Remove, `hu.py nor install remove-boot|remove-firmware --write`).

**Remove the boot path (stage 1 undo)** puts the stock boot back: the 44 environment bytes become exactly `bootcmd=bootsp\0bootdelay=0\0baudrate=921600\0\0` again at the same place (the stock form fills the whole 44-byte span, so its original padding comes back too), both xboot sums are recomputed, and the script block 0xE00000 is erased. From the next power-on only stock eCos starts, even with a card in; this app comes back only by setting it up again from a PC (`tools/patch_uboot.py` + `tools/flash_bootcmd.py`) or an SD-card ISP update. It uses the same planner as the install (`inst_plan_unboot`), the same `nor_boot_write` guard (which now also accepts an all-0xFF script block) and the same write order: script block, environment block(s), header block. Refusals, on top of the U-Boot checks of the install:

- the environment must be our patched form, exactly once, and the stock form nowhere ("nothing to remove: U-Boot's environment is stock", "... found more than once", "... not found");
- the script block must hold our own valid script, version 1 or 2 ("the boot script block does not hold our script (version 1 or 2)"): blank, foreign, half-written and newer scripts are all refused;
- the planned result, read back through the overlay, must pass xboot's check with the stock environment exactly once at the same place.

A copy in the stage 2 region is left alone; it is simply never loaded.

**Remove the flash copy (stage 2 undo)** only touches our own copy: committed, not yet committed, or half removed (header CRC valid and nothing past the image's end); a blank region is "nothing to remove", anything else "not ours". It first programs the magic word to 0 (programming only clears bits, no erase), so the copy can no longer boot while the header still marks the region as ours, then erases the blocks that hold data, last block first and the header block last, and ends when the whole region reads blank. Refused while any install or removal is being written.

The host test undoes the fresh install and this unit's original state (patched environment, version 1 script) and gets dump1.bin back byte for byte (environment and header blocks equal, script block blank), undoes the three shifted U-Boot layouts, round-trips undo + install, checks the refusals above, and removes copies in every state.

## The write guard (nor.c)

The general erase and program paths (`nor_erase`, `nor_program`, HU1 sub-ops 4..6) still accept only the free ranges. Stage 1 goes through `nor_boot_write` only, which:

- accepts exactly three kinds of 64 KB block (`nor_boot_block_ok`): 0xE00000, the block holding the uImage header at 0x8A40E0, and the block(s) holding the environment, whose location must lie inside xboot's largest U-Boot body (0x8A4120..0xAA40DF, asserted below the free region);
- re-reads the live block and requires the CRC the plan saw, with the new block differing only in the environment bytes and the two sum words (the script block: the new one must be a uImage); or, for a retry after an interrupted write, a live block made only of 0xFF and bytes of the new block; a block that already equals the new one is skipped;
- lets exactly that block through the per-unit check (`unit_ok`) while it erases and programs it, page 0 last, then verifies through the controller and the XIP window.

No U-Boot code runs in either stage: reads, erases and programs use the payload's own driver, never `sfsp`, so U-Boot's `sfsp` state at DRAM 0x800 (inside the H.264 ring) is never touched. U-Boot runs only for the dry run's `source`.

## How it runs

Planning runs on the net task when the request arrives (about 3 MB of controller reads, plus `BOOT.BIN` from the card for stage 2). Writing then runs one bounded step per net-loop pass (`install_poll`): one 64 KB boot block for stage 1; for stage 2 one block erase, the header, one 64 KB image chunk (blank check, program, verify), the whole-region CRC, or the magic. HU1 keeps answering between steps and `hu.py nor install progress` reports the step. AA video stalls during each step, so install with no AA session. Buffers: `MM_INSTALL` (0x02B80000, 3.5 MB, between the LVGL draw buffer and the EXEC trampoline).

After the last step everything is re-read from flash and planned again: stage 1 counts as done only if the new plan has nothing left to write; stage 2 only if the region scan finds a committed copy with the new install counter whose span CRC matches; the stage 1 undo only if the environment reads stock and the script block blank; the stage 2 undo only if the region reads blank.

## Power loss and failures, step by step

Stage 1 on a fresh unit writes 0xE00000, then the environment block, then the header block (the order `tools/flash_bootcmd.py` used):

| Interrupted during / after | Next power-on |
|---|---|
| script block erase or program | environment still stock: `bootsp`, stock eCos. A retry accepts the half-written script block |
| script written, environment not yet touched | stock eCos (the script is never sourced) |
| environment block erase, program or verify | **U-Boot fails xboot's sum check: does not boot. CH341A + the original dump** |
| environment written, header block not yet (or half) written | **same: body and header sums disagree. CH341A** |
| header block written and verified | boots the version 2 script |

Stage 1 undo (remove-boot) writes the same blocks in the same order:

| Interrupted during / after | Next power-on |
|---|---|
| script block erase | environment still patched: `source` rejects the blank or half-erased script, `bootsp`, stock eCos |
| script erased, environment not yet touched | stock eCos (the same) |
| environment block erase, program or verify | **U-Boot fails xboot's sum check: does not boot. CH341A + the original dump** |
| environment written, header block not yet (or half) written | **same. CH341A** |
| header block written and verified | stock eCos only (the intended result) |

The undo also stops the payload from starting at power-on after its first step, so a power loss there cannot be retried from the payload: the boot path comes back from a PC or an SD-card update. A failure while the payload keeps running keeps the plan, as for the install: press Remove (or `install remove-boot --write`) again before powering off.

The window that can stop the unit from booting is two 64 KB erase + program cycles (about 1 to 3 s). The same window existed with `flash_bootcmd.py`. If a step fails while the payload keeps running (a verify mismatch, the guard refusing), the plan is kept: the status says "STAGE 1 INTERRUPTED", stage 2 is refused, and running `install boot` again (or pressing Set up) continues from the failed block; `nor_boot_write` accepts blocks it already wrote or half wrote. Do not power off in that state. A timeout sets the driver's sticky fault (no writes until a reboot), and a reboot in the middle of the U-Boot blocks means the CH341A.

On this unit (environment already patched) stage 1 writes only 0xE00000. A power loss during that block leaves either the old script or one that `source` rejects (the header page is written last, both CRCs are checked), so the unit boots either as before or stock eCos, never a half-written script. In the stock case the card is no longer read at power-on, so the payload cannot retry by itself: recovery is an SD-card ISP round (`isp/mkisp.py`) that rewrites the script block with `sfsp`, or the CH341A. If the step fails while the payload keeps running, run stage 1 again before powering off.

Stage 2 never touches anything the unit needs to boot:

| Interrupted during | Next power-on, and the retry |
|---|---|
| erase (last block first, header block last) | an old copy's header may survive with its magic, but its span CRC fails: SD card, else stock. Retry: the region is still ours |
| header (written without the magic), image, whole-region check | no magic: SD card, else stock. Retry: the pending header marks the region as ours |
| magic word | a partial magic does not match: SD card, else stock |

Stage 2 undo (remove-firmware):

| Interrupted during | Next power-on, and the retry |
|---|---|
| zeroing the magic | the magic no longer matches (or still does, with a valid copy: it boots as before). Retry: still ours |
| erase (last block first, header block last) | magic 0: SD card, else stock. Retry: the header still marks the region as ours |

A copy only boots after its magic is programmed, which happens only after the whole region read back through the controller and through the XIP window gave the span CRC in the header. If a region ever reads as foreign after an interrupted header write, inspect it with `hu.py nor read` and clear it with `hu.py nor erase64 OFF --write` (the region is inside the free ranges).

## HU1 and hu.py

Op 29 sub-ops: 9 status `[src len crc]`, 10 boot `magic`, 11 firmware `magic [src len crc]`, 12 progress, 13 script, 14 remove-boot `magic`, 15 remove-firmware `magic`. Writes carry the NOR magic 0x57524F4E.

```
hu.py nor install status              # read-only; stage 2's dry run uses the card's BOOT.BIN
hu.py nor install status NET.BIN      # same, stage 2's dry run with this file
hu.py nor install script              # the script text; rebuilt on the PC and compared with the payload's image
hu.py nor install dryrun [nor|sd]     # see below
hu.py nor install boot --write        # stage 1, then polls progress
hu.py nor install firmware [NET.BIN] --write   # stage 2 (without a file: the card's BOOT.BIN, the running build)
hu.py nor install remove-boot --write      # undo stage 1: stock environment and sums, script erased
hu.py nor install remove-firmware --write  # undo stage 2: erase the copy
hu.py nor install progress
```

Without `--write`, `boot`, `firmware`, `remove-boot` and `remove-firmware` print the status (the dry run of what each stage and each undo would do) and exit.

### Settings > Flash

Each stage has a row that shows whether it is applied. Stage 1: "Set up" when it is not, "set up" plus Remove when it is. Stage 2: Install when there is no copy; "Installed (build X)" plus Remove when the copy is the running build; Update plus Remove when it is another build. Every button asks first; the stage 1 Remove dialog says that the original firmware starts from the next power-on and this app no longer does until it is set up again from a PC or an SD-card update. The rows and the status line are prepared by the net task when the page opens and after each step; the status line no longer shows the install counter (`hu.py nor install status` does).

### Script dry run

`hu.py nor install dryrun` fetches the script text from the payload, checks that it rebuilds to the payload's uImage, turns every side effect into an `echo DRY ...` (pinmux, UART and MCU frame writes, `setenv isp_if` / `isp_dev`, `mmc`, `wdt stop all`, `go`), replaces the two `fatload ... /BOOT.BIN` with `false` (variant `nor`) or `true` (variant `sd`), moves the copy target from 0x1800000 to 0x03100000, refuses to run if any unsafe command is left, uploads it to 0x03000000 and runs `source 0x3000000` through the CMD op. It runs both variants unless one is named. Expected:

- `sd`: `DRY go 0x3100000 hello`, `DRY go 0x3100000 main`, `DRY result hu_ok=1 hu_src=sd`.
- `nor` before stage 2: the `cmp.l` of the magic fails (a "word at ... != ..." line), no `DRY go`, `DRY result hu_ok= hu_src=`.
- `nor` after stage 2: the `crc32` lines print the span CRC, both `cmp.l` lines say the words are the same, `DRY go ...`, `DRY result hu_ok=1 hu_src=nor`; 0x03100000 then holds the image (`hu.py read 0x3100000 LEN` and compare with the file).

It reads the XIP window from the net task (no writes are running then) and changes only `hu_ok`, `hu_src` and `hu_nor` in U-Boot's environment, which the payload reads only at boot.

## On-unit test plan

Prerequisites: no AA session, a fresh backup (`hu.py nor dump` or Settings > System > Back up), the CH341A and the original dump at hand.

1. `hu.py exec NET-install.BIN` (build 20261109-143336). `hu.py nor install status`. Expect: flash 204018; U-Boot body b8dd0, sums ok, env **patched** at 92f09b; script block "version 1 (SD card only)"; stage 1 "would write e00000 (script v2)"; copy region blank; stage 2 "REFUSE: stage 1 is not installed". Anything else: stop and compare with this note.
2. `hu.py nor install script`: "rebuilt here: identical". `hu.py nor install dryrun`: the outputs above (the `nor` variant must not `go`). This proves the hush syntax, `test -z`, `cmp.l` return codes and `crc32`'s store on this U-Boot before anything is written.
3. Owner OK, then `hu.py nor install boot --write`: one block, "last: ok ... installed". `hu.py nor install status`: script "ours, version 2", stage 1 "nothing to do". `hu.py nor compare 0xE00000 0x10000`: controller = xip.
4. Power cycle with the card in: the unit boots the SD card as before. `hu.py cmd 'printenv hu_src'`: `hu_src=sd`.
5. Persist NET-install.BIN as BOOT.BIN if it is not already the running build (`hu.py persist`), so the UI and the status path use the card file; or use `hu.py nor install firmware NET-install.BIN --write` directly. `hu.py nor install status` first: stage 2 "would write build ..., erasing 0 block(s)". Then `hu.py nor install firmware --write`: about 22 steps, "last: ok". `hu.py nor install status`: "copy #1, build ..., CRC ok: boots". `hu.py nor install dryrun nor`: `DRY result hu_ok=1 hu_src=nor`.
6. Rename the card's BOOT.BIN (`hu.py fs` or on the PC) and power-cycle: the NOR copy boots. `hu.py cmd 'printenv hu_src'`: `hu_src=nor`. Check the UI comes up and Settings > Flash reads "Boot: SD card, flash copy (build ...), stock", with the stage 2 row "Installed (build ...)" and Remove.
7. Restore BOOT.BIN, power-cycle, confirm the SD path again.

Undo (with the owner's OK; the stage 1 undo leaves the unit on stock eCos until it is set up again):

8. `hu.py nor install remove-firmware` (dry run: "would zero the magic, then erase N block(s)"), then with `--write`: "last: ok", region blank. Settings > Flash: stage 2 row back to Install. Reinstall with `install firmware --write`.
9. `hu.py nor install remove-boot` (dry run: "would write e00000 (script erased) 920000 (stock environment) 8a0000 (uImage sums)", sums back to the original dump's). Running it with `--write` makes the next power-on start stock eCos only. Until that power-on the payload is still running, and `install boot --write` puts the boot path back (a fresh-unit plan, three blocks); after it, the way back is a PC flash with `tools/flash_bootcmd.py` or an SD-card ISP update. Only power off after the undo when that is ready.

Stage 1 on a fresh unit (environment stock) is the same plan with three blocks; the host test shows it equals `tools/patch_uboot.py`'s output with the script swapped, byte for byte.

## Version tolerance

Nothing about U-Boot's internals is hard-coded except the header address 0x8A40E0 (where xboot loads it from) and the two 44-byte environment forms: the body size, the environment's place, which blocks hold it (one, two across a boundary, or the header's own block) all come from the live image, and every change is checked against xboot's sums before and after. The host test covers the original dump, the patched dump, the environment moved into the next block, across a block boundary and into the header block, and refusals for a missing, duplicated or both-forms environment, a damaged header or body sum, a missing verify flag, foreign or newer data in the script block, and foreign data, junk past the copy, a damaged header, an oversized or header-less image in the copy region.

## Open questions

- The dry run on the unit is the first execution of the version 2 script's NOR branch (nested `if` with `cmp.l`, `crc32` storing to memory, `cp.l` from the XIP window).
- With BOOT.BIN missing, `isp_dev` stays 0 when the NOR copy boots, even if the card sits in slot 1. The payload's U-Boot-side card access (`fatload`/`fatwrite $isp_if $isp_dev`) then points at slot 0; its own SD driver is not affected.
- The magic word is programmed into a page that was already programmed (partial page program). Standard for SPI NOR; the on-unit read-back confirms it on this part.
- Program speed for 64 KB through the driver is not measured yet; the step sizes assume well under a second per chunk.

## On-unit results

- 2026-11-09, build 20261109-143623 (EXEC'd): `install status` as expected (env patched at 92f09b, script v1, region blank); `install script` rebuilt identical; dry run `sd` → `go` + `hu_ok=1 hu_src=sd`, `nor` (before stage 2) → magic mismatch, no `go`. Stage 1 (`install boot --write`): one block, 316 ms, verified, script block now "nor boot script v2". The next boot (an unrelated reset of the EXEC'd build) and a later power cycle both came up through the v2 script from the SD card (`hu_src=sd`).
- Build 20261109-151530 (cold boot from the card): stage 2 (`install firmware --write`) wrote copy #1, 1192144 bytes, CRC 5a6cfaba, region status "CRC ok: boots". Dry run `nor` after stage 2: all three `cmp.l` same, `go` + `hu_ok=1 hu_src=nor`, and the image copied to 0x03100000 has CRC 5a6cfaba, the card's BOOT.BIN. Not yet done: a real power-on with no BOOT.BIN on the card.
- Cold boots of builds 20261109-151530 / -152258 lost AA's first picture (decoder timeout, then "list modification names a missing frame" until the next IDR): the payload ran U-Boot's `test "$hu_src" = ...` at boot to report the boot source. Without those two calls (20261109-152634) the same code decodes cleanly after a cold boot; the boot source is now read with `hu.py cmd 'printenv hu_src'`. Stage 2 rewritten as copy #2 of 20261109-152634 (41 steps, 13.5 s, verified).
- No-BOOT.BIN test: BOOT.BIN deleted from the card, power cycle: the unit came up on build 20261109-152634 from the NOR copy (`hu_src=nor`), joined Wi-Fi, and ran AA (797 pictures, 0 decode errors). BOOT.BIN put back with `hu.py persist`; the next power cycle booted from the card again (`hu_src=sd`).
