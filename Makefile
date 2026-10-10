# opera-boot - minimalist open 3DO ROM that always boots the CD OS.
# GPL-2.0-only bootstrap code; see docs/LICENSE. Splash artwork is third-party.

# Toolchain: 3DO devkit ports of ARM SDT (armcc 4.91 / armasm 2.50 /
# armlink 5.20), supplied by a sourced DevKit activate-env.
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(strip $(TDO_DEVKIT_PATH)),)
$(error Source /path/to/3do-devkit/activate-env before building)
endif
endif

CC       ?= gcc
HOSTCFLAGS ?= -O2 -Wall -Wno-format

BUILD    := build
BUILD_DATE ?= $(shell date -u +%Y-%m-%d)

# SDT 2.51 wants `-apcs /noswstackcheck` (space) for armcc.
ARMCCFLAGS := -bi -fa -za1 -wp -Ospace -apcs /noswstackcheck -arch 3
ARMASFLAGS := -bigend -Apcs 3/32bit/noswst

# ROM image layout (mkrom also knows these; keep in sync).
LAYOUT_ROMTAGS := 0x01000
DIPIR_RAM      := 0x200

MINI_CSRC := util.c mei.c label.c loader.c display.c minidipir.c oldmisc.c
MINI_OBJS := $(BUILD)/b/vectors.o $(MINI_CSRC:%.c=$(BUILD)/b/%.o) \
	$(BUILD)/b/rtdiv.o

.PHONY: all clean FORCE
all: $(BUILD)/opera-boot.bin

$(BUILD) $(BUILD)/b:
	mkdir -p $@

$(BUILD)/b/vectors.o: src/vectors.s | $(BUILD)/b
	armasm $(ARMASFLAGS) $< -o $@

$(BUILD)/b/rtdiv.o: src/rtdiv.s | $(BUILD)/b
	armasm $(ARMASFLAGS) $< -o $@

$(BUILD)/b/%.o: src/%.c src/opera-boot.h | $(BUILD)/b
	armcc $(ARMCCFLAGS) -Isrc -I$(BUILD) -c $< -o $@

$(BUILD)/b/banner.o: src/logo_data.h $(BUILD)/banner_caption.h

# Check each invocation; unchanged header bytes keep their timestamp.
$(BUILD)/banner_caption.h: tools/gen_caption.py FORCE | $(BUILD)
	python3 tools/gen_caption.py "$(BUILD_DATE)" $@

# The vector table must be the first 44 bytes: vectors.o links first.
$(BUILD)/minidipir.bin: $(MINI_OBJS)
	armlink -bin -ro-base $(DIPIR_RAM) -map -o $@ $(MINI_OBJS) \
	  > $(BUILD)/minidipir.map 2>&1 || { cat $(BUILD)/minidipir.map; exit 1; }
	@# image + 2KiB private stack must stay below the component area.
	@python3 tools/mapcheck.py $(BUILD)/minidipir.map 0x8000

# Launch constants plus the 4-padded resident-blob copy length.
$(BUILD)/launch_defs.i: Makefile $(BUILD)/minidipir.bin | $(BUILD)
	@printf 'DIPIR_BLOB_ADDR\tEQU\t0x03002000\n' > $@
	@printf 'FAKEHDR_ADDR\tEQU\t0x0301F000\n' >> $@
	@printf 'ROMTAGS_ADDR\tEQU\t0x03001000\n' >> $@
	@sz=$$(wc -c < $(BUILD)/minidipir.bin); \
	  printf 'DIPIR_BLOB_LEN\tEQU\t%s\n' "$$(( (sz + 3) / 4 * 4 ))" >> $@
	@printf '\tEND\n' >> $@

$(BUILD)/b/launch.o: launch/launch.s $(BUILD)/launch_defs.i | $(BUILD)/b
	armasm $(ARMASFLAGS) -I$(BUILD) $< -o $@

$(BUILD)/launch.bin: $(BUILD)/b/launch.o $(BUILD)/b/banner.o
	armlink -bin -ro-base 0x03000000 -o $@ $^

$(BUILD)/romtags_defs.i: $(BUILD)/minidipir.bin | $(BUILD)
	@sz=$$(wc -c < $<); \
	  printf 'DIPR_SIZE EQU %s\nDIPRBIN_FILEOFF EQU 0x2000\n\tEND\n' "$$sz" > $@

$(BUILD)/romtags.bin: launch/romtags.s $(BUILD)/romtags_defs.i
	armasm $(ARMASFLAGS) -I$(BUILD) $< -o $(BUILD)/romtags.o
	@# The table's entry is linker metadata only; BIN output has no executable header.
	armlink -bin -ro-base $(LAYOUT_ROMTAGS) -entry $(LAYOUT_ROMTAGS) \
	  -o $@ $(BUILD)/romtags.o

$(BUILD)/mkrom: tools/mkrom.c | $(BUILD)
	$(CC) $(HOSTCFLAGS) -o $@ $<

# Single self-contained 128 KiB ROM.  Every referenced byte, including
# the zero-version fake OS header at 0x1F000, is present in the file.
$(BUILD)/opera-boot.bin: $(BUILD)/mkrom $(BUILD)/launch.bin \
    $(BUILD)/romtags.bin $(BUILD)/minidipir.bin
	$(BUILD)/mkrom $@ $(BUILD)/launch.bin $(BUILD)/romtags.bin \
	  $(BUILD)/minidipir.bin > $(BUILD)/hashes.txt
	@cat $(BUILD)/hashes.txt

FORCE:

clean:
	rm -rf $(BUILD)
