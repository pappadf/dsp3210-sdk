ASM    := ../dsp3210asm
DIS    := ../dsp3210dis
EMU    := ../dsp3210emu
CC     ?= cc
CFLAGS ?= -O2 -g
LDLIBS += -lm

# Mandatory flags live here, not in CFLAGS, so that a command-line
# `make CFLAGS=...` (as distro packagers and sanitizer builds use)
# adds to the build rather than replacing the language and -I flags.
ALL_CFLAGS := -std=c99 -Wall -Wextra -pedantic \
              -I$(ASM) -I$(DIS) -I$(EMU) $(CFLAGS)

APPS   := $(sort $(wildcard apps/*.s))

# run_apps is built once per DAU implementation (same emulator core
# source, compiled with/without -DDSP3210_DAU_EXACT — see
# $(EMU)/dsp3210_emu.c).
EMUDEPS := $(EMU)/dsp3210_emu.c $(EMU)/dsp3210_emu.h \
           $(EMU)/dsp3210_dau_double.inc $(EMU)/dsp3210_dau_exact.inc

all: roundtrip run_apps run_apps_exact

dsp3210_asm.o: $(ASM)/dsp3210_asm.c $(ASM)/dsp3210_asm.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

dsp3210_dis.o: $(DIS)/dsp3210_dis.c $(DIS)/dsp3210_dis.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

dsp3210_emu.o: $(EMUDEPS)
	$(CC) $(ALL_CFLAGS) -c -o $@ $(EMU)/dsp3210_emu.c

dsp3210_emu_exact.o: $(EMUDEPS)
	$(CC) $(ALL_CFLAGS) -DDSP3210_DAU_EXACT -c -o $@ $(EMU)/dsp3210_emu.c

roundtrip: roundtrip.c dsp3210_asm.o dsp3210_dis.o
	$(CC) $(ALL_CFLAGS) -o $@ roundtrip.c dsp3210_asm.o dsp3210_dis.o \
	    $(LDFLAGS) $(LDLIBS)

run_apps: run_apps.c dsp3210_asm.o dsp3210_dis.o dsp3210_emu.o
	$(CC) $(ALL_CFLAGS) -o $@ run_apps.c dsp3210_asm.o \
	    dsp3210_dis.o dsp3210_emu.o $(LDFLAGS) $(LDLIBS)

run_apps_exact: run_apps.c dsp3210_asm.o dsp3210_dis.o dsp3210_emu_exact.o
	$(CC) $(ALL_CFLAGS) -o $@ run_apps.c dsp3210_asm.o \
	    dsp3210_dis.o dsp3210_emu_exact.o $(LDFLAGS) $(LDLIBS)

test: all
	./roundtrip $(APPS)
	./run_apps $(APPS)
	./run_apps_exact $(APPS)

clean:
	rm -f *.o roundtrip run_apps run_apps_exact

.PHONY: all test clean
