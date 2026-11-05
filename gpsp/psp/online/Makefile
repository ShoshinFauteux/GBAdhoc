# gbadhoc_online.prx -- EXPERIMENTAL online play as an on-demand USER module
# (online_link.h, docs/ONLINE-TUNNEL.md section 8).  Built as a sub-make from
# psp/Makefile when ONLINE=1, ships beside EBOOT.PBP, loaded with
# sceKernelLoadModule only for an online session and unloaded after it.
TARGET = gbadhoc_online
OBJS = main.o transport_relay.o prx_libc.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -I. -I../../netdrv
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

# No libc and no start files: the module takes no heap of its own (memory and
# formatting come from the EBOOT, prx_libc.h).  -nostdlib also drops the
# spec's library tail, so the stubs this module needs are named here once
# each -- the stub-order trap is a library linked TWICE; none is, here.
LDFLAGS = -nostartfiles -nostdlib
LIBS = -lpspnet_resolver -lpspnet_inet -lpspmodinfo -lpspsdk -lpspkernel -lgcc

PSP_FW_VERSION = 500

# prx_libc.h renames calloc/free/printf-family before any libc header (not
# for the generated exports.c, which defines its own NULL)
main.o transport_relay.o prx_libc.o: CFLAGS += -include prx_libc.h
# the string routines themselves: gcc would turn their loops back into calls
# to memset/memcpy -- that is, into themselves (infinite recursion)
prx_libc.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

transport_relay.o: ../../netdrv/transport_relay.c ../../netdrv/transport_relay.h ../../netdrv/relay_proto.h
	$(CC) $(CFLAGS) -c -o $@ $<
