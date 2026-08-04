DIS     := ../dsp3210dis
CC      ?= cc
CFLAGS  ?= -O2
LDLIBS  += -lm
AR      ?= ar

# Mandatory flags live here, not in CFLAGS, so that a command-line
# `make CFLAGS=...` (as distro packagers and sanitizer builds use)
# adds to the build rather than replacing the language and -I flags.
ALL_CFLAGS := -std=c99 -Wall -Wextra -pedantic -I$(DIS) $(CFLAGS)

all: libdsp3210asm.a dsp3210asm

dsp3210_asm.o: dsp3210_asm.c dsp3210_asm.h
	$(CC) $(ALL_CFLAGS) -c -o $@ dsp3210_asm.c

libdsp3210asm.a: dsp3210_asm.o
	$(AR) rcs $@ dsp3210_asm.o

dsp3210asm: dsp3210asm.c libdsp3210asm.a
	$(CC) $(ALL_CFLAGS) -o $@ dsp3210asm.c libdsp3210asm.a \
	    $(LDFLAGS) $(LDLIBS)

dsp3210_dis.o: $(DIS)/dsp3210_dis.c $(DIS)/dsp3210_dis.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

test_dsp3210_asm: test_dsp3210_asm.c libdsp3210asm.a dsp3210_dis.o
	$(CC) $(ALL_CFLAGS) -o $@ test_dsp3210_asm.c libdsp3210asm.a \
	    dsp3210_dis.o $(LDFLAGS) $(LDLIBS)

test: test_dsp3210_asm
	./test_dsp3210_asm

clean:
	rm -f *.o *.a dsp3210asm test_dsp3210_asm

.PHONY: all test clean
