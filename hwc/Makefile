#!/usr/bin/make -f
# hwc/Makefile - build the software composer module two ways.
#
#   make            guest module for arm64 (cross compiler, no AOSP tree)
#   make check      syntax-check against the vendored shim headers
#   make platform   real build inside an AOSP tree (uses platform headers)
#
# The AOSP-tree build is the one you ship: it gets the authoritative
# <hardware/hwcomposer2.h> and libui's GraphicBufferMapper, so descriptor
# numbers and buffer mapping are the platform's, not our approximation.
AARCH64_CC ?= aarch64-linux-gnu-gcc
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
INC := -Iinclude

SRC := src/hwcomposer_qandroid.c

.PHONY: all check platform clean

all: hwcomposer.qandroid.so

hwcomposer.qandroid.so: $(SRC) include/*.h
	$(AARCH64_CC) $(CFLAGS) -fPIC -shared $(INC) -o $@ $(SRC)

check:
	$(CC) -fsyntax-only $(CFLAGS) $(INC) $(SRC) && echo "hwc: syntax clean"

# Inside an AOSP checkout:  mmm hardware/qandroid/hwc   (Android.bp below is for that)
platform:
	@echo "drop this directory into hardware/qandroid/hwc and use Android.bp"
	@ls Android.bp 2>/dev/null || echo "no Android.bp here - see docs/HWC.md"

clean:
	rm -f hwcomposer.qandroid.so
