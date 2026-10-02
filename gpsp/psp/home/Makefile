# gbadhoc_home.prx -- kernel module for Settings > Controls > Menu = HOME
# (docs/CONTROL-REMAP.md section 10).  Built as a sub-make from psp/Makefile,
# ships beside EBOOT.PBP, loaded at runtime with kuKernelLoadModule only when
# a game starts with the menu on HOME.  Independent of gbadhoc_me.prx on
# purpose: the Media Engine module stays byte-for-byte what it was.
TARGET = gbadhoc_home
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

LDFLAGS = -nostartfiles
# sceImpose_driver stubs live in libpspkernel.a; sceCtrl_driver comes from
# build.mak's kernel tail (-lpspctrl_driver).  Do NOT name either here: the
# stub-order trap (a library listed twice breaks imports as a WARNING).
LIBS = -lpspkernel

PSP_FW_VERSION = 500

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
