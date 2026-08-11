TARGET = hello
OBJS = main.o

CFLAGS = -O2 -Wall
LIBS = -lpsppower

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = gpsp-adhoc harness hello

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
