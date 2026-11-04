# psp/launcher/Makefile -- the per-game XMB launcher (issue #29,
# docs/XMB-LAUNCH.md).  Build in the pspdev image:
#   docker run --rm -v <repo>:/build -w /build/gpsp/psp/launcher pspdev/pspdev make
# The EBOOT.PBP built here is the TEMPLATE the entry makers (tools/xmbwrap)
# take DATA.PSP from; each entry gets its own PARAM.SFO, ICON0 and PIC1.
TARGET = launcher
OBJS   = main.o
CFLAGS = -O2 -G0 -Wall -Wextra
ASFLAGS = $(CFLAGS)
LIBS   = -lpspsystemctrl_user
EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = GBAdhoc game

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
