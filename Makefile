# Manatee (Dreamcast AICA sound driver) - logical C reconstruction, Ghidra project and explainer site.
#
#   make          compile every C file for ARMv4 (compile check; not a byte-matching build)
#   make link     link the whole driver (proves every call resolves)
#   make ghidra   build the annotated Ghidra project in ghidra/
#   make site     rebuild the explainer site in site/
#
# src/arm/tables.c holds constant tables copied from the driver image, so it is generated from
# your own bin/manatee_arm.bin (see README) instead of being committed.
CC      ?= clang
ARMFLAGS = --target=arm-none-eabi -march=armv4 -marm -ffreestanding -fno-builtin -nostdlib \
           -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc/arm
SRCS    := $(sort $(wildcard src/arm/*.c) src/arm/tables.c)
OBJS    := $(patsubst src/arm/%.c,build/arm/%.o,$(SRCS))
IMAGE   := bin/manatee_arm.bin

all: $(OBJS)

src/arm/tables.c: $(IMAGE) tools/gen_tables.py
	python3 tools/gen_tables.py

$(IMAGE):
	@echo "error: $(IMAGE) not found. Extract it from your game first:" >&2
	@echo "       python3 tools/extract_sdrv.py path/to/CINIT.DAT" >&2
	@false

build/arm/%.o: src/arm/%.c $(wildcard src/arm/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(ARMFLAGS) -c $< -o $@

link: $(OBJS)
	$(CC) --target=arm-none-eabi -march=armv4 -nostdlib -fuse-ld=lld -Wl,--entry=drv_reset \
	    -Wl,--error-limit=0 -Wl,--image-base=0 $(OBJS) -o build/arm/manatee.elf

ghidra: $(IMAGE)
	tools/ghidra/build_project.sh

site:
	python3 tools/build_site.py

clean:
	rm -rf build src/arm/tables.c

.PHONY: all link ghidra site clean
