#!/usr/bin/make -f
# qandroid - arm64 Android on x86_64 Linux
PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
HOSTCC := $(CC)

.PHONY: all tools shim test check clean doctor

all: tools client

tools: tools/binder-mk tools/binder-ver

tools/binder-mk: tools/binder-mk.c
	$(HOSTCC) $(CFLAGS) -o $@ $<

tools/binder-ver: tools/binder-ver.c
	$(HOSTCC) $(CFLAGS) -o $@ $<

# The shim is guest-arch code: build it with an aarch64 cross compiler.
AARCH64_CC ?= aarch64-linux-gnu-gcc
shim: shim/libabshim.so

shim/libabshim.so: shim/abshim.c
	$(AARCH64_CC) -O2 -fPIC -shared -o $@ $<

# ---- display backend -----------------------------------------------------
hwc-check:
	$(MAKE) -C hwc check

hwc-guest:
	$(MAKE) -C hwc

frontend/hwc_client: frontend/hwc_client.c hwc/include/qandroid_hwc_proto.h
	$(CC) $(CFLAGS) -Ihwc/include -o $@ frontend/hwc_client.c \
	    $$(pkg-config --cflags --libs sdl2)

client: frontend/hwc_client

test check:
	python3 tests/test_tools.py -v
	python3 tests/test_cli.py -v
	python3 tests/test_hwc_proto.py -v
	python3 tests/test_rootfs_build.py -v
	python3 tests/test_packaging.py -v

deb:
	./packaging/build-deb.sh

doctor:
	python3 ./qandroid doctor

clean:
	rm -rf dist
	rm -f tools/binder-mk tools/binder-ver shim/libabshim.so shim/*.o frontend/hwc_client \
	    hwc/hwcomposer.qandroid.so

install: all
	install -d $(DESTDIR)$(PREFIX)/lib/qandroid
	cp -a qandroid tools scripts patches frontend shim $(DESTDIR)$(PREFIX)/lib/qandroid/
	ln -sf $(PREFIX)/lib/qandroid/qandroid $(DESTDIR)$(PREFIX)/bin/qandroid
