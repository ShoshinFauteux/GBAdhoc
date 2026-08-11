# netdrv/Makefile — desktop build of the netpacket driver + unit tests.
#
#   make          -> libnetdrv.a (netdrv core + UDP transport)
#   make test     -> build and RUN the unit-test suite (desktop, no SDL)
#   make clean
#
# The netdrv core (netdrv.c) is transport-agnostic plain C and is also
# compiled into the PSP frontend later (Phase 4) against transport_adhoc.
# transport_udp.c is desktop-only (POSIX sockets). Run under WSL/Linux.

CC      ?= gcc
CFLAGS  := -O2 -g -Wall -Wextra -I.

LIB     := libnetdrv.a
OBJS    := obj/netdrv.o obj/transport_udp.o

all: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)

obj/%.o: %.c netdrv.h netdrv_wire.h transport_udp.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/test_netdrv.o: tests/test_netdrv.c netdrv.h netdrv_wire.h transport_udp.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

test_netdrv: obj/test_netdrv.o $(LIB)
	$(CC) -o $@ obj/test_netdrv.o $(LIB)

test: test_netdrv
	./test_netdrv

clean:
	rm -rf obj $(LIB) test_netdrv

.PHONY: all test clean
