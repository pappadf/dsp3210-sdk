SUBDIRS := dsp3210dis dsp3210asm dsp3210emu toolchain-tests

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d || exit 1; done

test:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d test || exit 1; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

.PHONY: all test clean
