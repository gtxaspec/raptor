# Variables
commit_tag=$(shell git rev-parse --short HEAD)

CC = ccache $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

CONFIG_MUSL_BUILD=y
CONFIG_STATIC_BUILD=n
DEBUG=n

ifneq ($(TARGET),t20)
TARGET=t31
endif

SDK_INC_DIR = include/$(TARGET)
INCLUDES = -I$(SDK_INC_DIR) -I./include
LIBS = $(SDK_LIB_DIR)/uclibc/libimp.$(LIBTYPE) $(SDK_LIB_DIR)/uclibc/libalog.$(LIBTYPE) \
	$(SDK_LIB_DIR)/uclibc/libsysutils.$(LIBTYPE)

CFLAGS = $(INCLUDES) -O2 -Wall -march=mips32r2
LDFLAGS += -Wl,-gc-sections
LDLIBS = -lpthread -lm -lrt -ldl
SDK_LIB_DIR = lib/$(TARGET)

ifeq ($(CONFIG_MUSL_BUILD), y)
CROSS_COMPILE ?= mipsel-linux-
endif

ifeq ($(DEBUG), y)
CFLAGS += -g # Add -g for debugging symbols
STRIPCMD = @echo "Not stripping binary due to DEBUG mode."
else
STRIPCMD = $(STRIP)
endif

ifeq ($(CONFIG_STATIC_BUILD), y)
LDFLAGS += -static
LIBTYPE=a
else
LIBTYPE=so
endif

ifeq ($(TARGET),t20)
CFLAGS += -DPLATFORM_T20 -DSENSOR_JXF23 -DSENSOR_FRAME_RATE_NUM=15 -DSOC=T20
else
CFLAGS += -DPLATFORM_T31 -DSENSOR_GC2053 -DSENSOR_FRAME_RATE_NUM=30 -DSOC=T31
endif

APP = raptor
raptor_OBJS = raptor.o encoder.o system.o musl_shim.o tcp.o

.PHONY:	all version clean distclean $(APP)

all: version $(APP)

version:
	@if  ! grep "$(commit_tag)" version.h >/dev/null 2>&1 ; then \
	echo "update version.h" ; \
	sed 's/COMMIT_TAG/"$(commit_tag)"/g' version.tpl.h > version.h ; \
	fi

$(APP): version $(raptor_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(raptor_OBJS) $(LIBS) $(LDLIBS)
	$(STRIPCMD) $@

%.o:%.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f version.h *.o *~

distclean: clean
	rm -f $(APP)
