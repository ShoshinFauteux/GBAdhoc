# Standalone TGB Dual GB/GBC smoke build.  The parent frontend adds these
# sources to its platform build (psp/Makefile); this file keeps the source
# set and the core's compile flags reproducible on a host.
#
# `smoke` compiles in one command so no host object lands next to the
# sources, where the PSP build keeps its MIPS objects of the same names.
#
# The vendored core needs -fgnu89-inline (it defines non-static `inline`
# functions and takes their addresses) and -fno-strict-aliasing (it reads
# VRAM and ROM bytes through word pointers).  -Wno-unused-parameter: the
# I/O and opcode dispatch tables give every handler one signature, and 157
# handlers ignore their argument.  Every other warning is fixed in the
# vendored files (tgbdual/LOCAL-PATCHES.md) and stays on.
CC ?= cc
CFLAGS ?= -O2
CORE_CFLAGS = -std=gnu11 -fgnu89-inline -fno-strict-aliasing -Wall -Wextra \
              -Wno-unused-parameter
GLUE_CFLAGS = -std=gnu11 -Wall -Wextra
CPPFLAGS += -I. -I../frontend-common

CORE_SOURCES = \
  tgbdual/apu.c \
  tgbdual/cpu.c \
  tgbdual/gb.c \
  tgbdual/lcd.c \
  tgbdual/mbc.c \
  tgbdual/rom.c \
  tgbdual/sgb.c   tgbdual/tgb_shared.c

all: smoke

smoke: gbcore_tgbdual.c $(CORE_SOURCES) tests/smoke.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c $(CORE_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLUE_CFLAGS) -o $@ tests/smoke.c \
	   gbcore_tgbdual.c $(notdir $(CORE_SOURCES:.c=.o))
	rm -f $(notdir $(CORE_SOURCES:.c=.o))

clean:
	rm -f smoke $(notdir $(CORE_SOURCES:.c=.o))

.PHONY: all clean
