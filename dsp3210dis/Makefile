CC      ?= cc
CFLAGS  ?= -O2
AR      ?= ar

# Mandatory flags live here, not in CFLAGS, so that a command-line
# `make CFLAGS=...` (as distro packagers and sanitizer builds use)
# adds to the build rather than replacing the language and -I flags.
ALL_CFLAGS := -std=c99 -Wall -Wextra -pedantic $(CFLAGS)

all: libdsp3210dis.a dsp3210dis

dsp3210_dis.o: dsp3210_dis.c dsp3210_dis.h
	$(CC) $(ALL_CFLAGS) -c -o $@ dsp3210_dis.c

libdsp3210dis.a: dsp3210_dis.o
	$(AR) rcs $@ dsp3210_dis.o

dsp3210dis: dsp3210dis.c libdsp3210dis.a
	$(CC) $(ALL_CFLAGS) -o $@ dsp3210dis.c libdsp3210dis.a $(LDFLAGS)

test_dsp3210: test_dsp3210.c libdsp3210dis.a
	$(CC) $(ALL_CFLAGS) -o $@ test_dsp3210.c libdsp3210dis.a $(LDFLAGS)

test: test_dsp3210
	./test_dsp3210

clean:
	rm -f *.o *.a dsp3210dis test_dsp3210

.PHONY: all test clean
