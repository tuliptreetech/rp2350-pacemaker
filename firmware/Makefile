# RP2350 Hazard3 (RISC-V) firmware, built with clang/lld — no vendor SDK, no
# riscv-gcc needed. Tool names are overridable so the same Makefile works both
# in the hydrate.py docker image (unsuffixed clang/ld.lld/llvm-objcopy from
# debian's clang+lld+llvm packages) and locally (versioned e.g. ld.lld-18).
CLANG ?= clang
LLD ?= ld.lld
OBJCOPY ?= llvm-objcopy

# start.S writes mtvec, so the ISA string has to allow CSR instructions — but
# how you say that depends on the compiler's age. Debian bookworm's clang 14
# (what hydrate's docker image installs) folds CSRs into base I and rejects
# "zicsr" as an extension name outright; newer clang accepts the explicit
# spelling. Probe for it rather than pinning either one.
ZICSR := $(shell $(CLANG) --target=riscv32-unknown-elf -march=rv32imac_zicsr \
                 -E -x c /dev/null >/dev/null 2>&1 && echo _zicsr)

# rv32imac is all this firmware needs, but the emulator's hazard3 SLEIGH
# architecture now implements the full silicon set (Zba/Zbb/Zbs/Zbkb, Zcb,
# Zcmp, Xh3bextm), so richer -march strings — or pico-sdk's own
# -mcpu=hazard3-rp2350 — are fine too.
CFLAGS = --target=riscv32-unknown-elf -march=rv32imac$(ZICSR) -mabi=ilp32 \
         -mcmodel=medany -Os -Wall -Wextra -ffreestanding -nostdlib

OBJS = start.o main.o

all: firmware.bin

%.o: %.c
	$(CLANG) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CLANG) $(CFLAGS) -c $< -o $@

firmware.elf: $(OBJS) rp2350.ld
	$(LLD) -T rp2350.ld $(OBJS) -o $@

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f $(OBJS) firmware.elf firmware.bin

.PHONY: all clean
