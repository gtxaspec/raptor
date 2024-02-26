# Variables
commit_tag=$(shell git rev-parse --short HEAD)

# Check if ccache is available
CCACHE_EXISTS := $(shell command -v ccache 2> /dev/null)

ifdef CCACHE_EXISTS
    CC = ccache $(CROSS_COMPILE)gcc
else
    CC = $(CROSS_COMPILE)gcc
endif

STRIP = $(CROSS_COMPILE)strip

CONFIG_MUSL_BUILD=y
CONFIG_STATIC_BUILD=n
DEBUG=n

ifeq ($(TARGET),t20)
SOC_FAMILY ?= T20
else
SOC_FAMILY ?= T31
TARGET=t31
endif

SDK_INC_DIR = include/$(TARGET)
INCLUDES = -I$(SDK_INC_DIR) -I./include
LIBS = $(SDK_LIB_DIR)/uclibc/libimp.$(LIBTYPE) $(SDK_LIB_DIR)/uclibc/libalog.$(LIBTYPE) \
	$(SDK_LIB_DIR)/uclibc/libsysutils.$(LIBTYPE)

CFLAGS = $(INCLUDES) -O2 -Wall -march=mips32r2 -DSOCKLEN_T=socklen_t -D_LARGEFILE_SOURCE=1 -D_FILE_OFFSET_BITS=64
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

CFLAGS += -DPLATFORM_$(SOC_FAMILY) -DSOC=$(SOC_FAMILY)
APP = raptor
raptor_OBJS = raptor.o encoder.o system.o musl_shim.o unix.o ini.o config.o framesource.o ringbuffer.o

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
