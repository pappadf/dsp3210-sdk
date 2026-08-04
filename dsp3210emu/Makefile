DIS    := ../dsp3210dis
CC     ?= cc
CFLAGS ?= -O2 -g
LDLIBS += -lm
AR     ?= ar

# Mandatory flags live here, not in CFLAGS, so that a command-line
# `make CFLAGS=...` (as distro packagers and sanitizer builds use)
# adds to the build rather than replacing the language and -I flags.
ALL_CFLAGS := -std=c99 -Wall -Wextra -pedantic -I$(DIS) $(CFLAGS)

# The core compiles twice: once per DAU implementation (see
# dsp3210_emu.c).  Everything else is shared source.
DAUDEPS := dsp3210_emu.c dsp3210_emu.h \
           dsp3210_dau_double.inc dsp3210_dau_exact.inc

all: libdsp3210emu.a libdsp3210emu-exact.a \
     dsp3210emu dsp3210emu-exact \
     test_dsp3210_emu test_dsp3210_emu_exact

dsp3210_emu.o: $(DAUDEPS)
	$(CC) $(ALL_CFLAGS) -c -o $@ dsp3210_emu.c

dsp3210_emu_exact.o: $(DAUDEPS)
	$(CC) $(ALL_CFLAGS) -DDSP3210_DAU_EXACT -c -o $@ dsp3210_emu.c

libdsp3210emu.a: dsp3210_emu.o
	$(AR) rcs $@ $^

libdsp3210emu-exact.a: dsp3210_emu_exact.o
	$(AR) rcs $@ $^

dsp3210_dis.o: $(DIS)/dsp3210_dis.c $(DIS)/dsp3210_dis.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

dsp3210emu.o: dsp3210emu.c dsp3210_emu.h $(DIS)/dsp3210_dis.h
	$(CC) $(ALL_CFLAGS) -c -o $@ dsp3210emu.c

dsp3210emu: dsp3210emu.o dsp3210_emu.o dsp3210_dis.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

dsp3210emu-exact: dsp3210emu.o dsp3210_emu_exact.o dsp3210_dis.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The test object is deliberately compiled ONCE, without
# -DDSP3210_DAU_EXACT, and linked against both cores: the accumulator
# slot is a union, so the two cores are ABI-identical and this link
# proves it.
test_dsp3210_emu.o: test_dsp3210_emu.c dsp3210_emu.h
	$(CC) $(ALL_CFLAGS) -c -o $@ test_dsp3210_emu.c

test_dsp3210_emu: test_dsp3210_emu.o dsp3210_emu.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_dsp3210_emu_exact: test_dsp3210_emu.o dsp3210_emu_exact.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test: test_dsp3210_emu test_dsp3210_emu_exact
	./test_dsp3210_emu
	./test_dsp3210_emu_exact

clean:
	rm -f *.o *.a dsp3210emu dsp3210emu-exact \
	    test_dsp3210_emu test_dsp3210_emu_exact

.PHONY: all test clean
