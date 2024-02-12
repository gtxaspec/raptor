# Variables
commit_tag=$(shell git rev-parse --short HEAD)

CC = ccache $(CROSS_COMPILE)gcc
CXX = ccache $(CROSS_COMPILE)g++
STRIP = $(CROSS_COMPILE)strip

CONFIG_MUSL_BUILD=y
CONFIG_STATIC_BUILD=n
DEBUG=n

CFLAGS = $(INCLUDES) -O2 -Wall -march=mips32r2
LDFLAG += -Wl,-gc-sections
SDK_LIB_DIR = lib
INCLUDES = -I$(SDK_INC_DIR)
LDLIBS = -lpthread -lm -lrt -ldl

ifeq ($(CONFIG_MUSL_BUILD), y)
CROSS_COMPILE ?= mipsel-linux-
endif

ifeq ($(DEBUG), y)
CFLAGS += -g # Add -g for debugging symbols
STRIPCMD = @echo "Not stripping binary due to DEBUG mode."
else
STRIPCMD = $(STRIP)
endif

#ifeq ($(CONFIG_STATIC_BUILD), y)
#LDFLAGS += -static
#LIBS = $(SDK_LIB_DIR)/libimp.a $(SDK_LIB_DIR)/libalog.a
#else
#LIBS = $(SDK_LIB_DIR)/libimp.so $(SDK_LIB_DIR)/libalog.so
#endif

ifeq ($(TARGET),t20)
SDK_INC_DIR = include/t20
LIBS = $(SDK_LIB_DIR)/t20/uclibc/libimp.so $(SDK_LIB_DIR)/t20/uclibc/libalog.so
COMPILE_OPTS += -DPLATFORM_T20
else
SDK_INC_DIR = include/t31
LIBS = $(SDK_LIB_DIR)/t31/uclibc/libimp.so $(SDK_LIB_DIR)/t31/uclibc/libalog.so
COMPILE_OPTS += -DPLATFORM_T31
endif

APP = raptor

.PHONY:all

all: 	$(APP)

$(APP): raptor.o encoder.o system.o musl_shim.o tcp.o
	$(CC) $(LDFLAG) -o $@ $^ $(LIBS) $(LDLIBS)
	$(STRIP) $@

%.o:%.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o *~

distclean: clean
	rm -f $(APP)
