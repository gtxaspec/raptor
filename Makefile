CONFIG_MUSL_BUILD=y

CROSS_COMPILE ?= mipsel-linux-

CC = $(CROSS_COMPILE)gcc
CPLUSPLUS = $(CROSS_COMPILE)g++
LD = $(CROSS_COMPILE)ld
AR = $(CROSS_COMPILE)ar cr
STRIP = $(CROSS_COMPILE)strip

CFLAGS = $(INCLUDES) -O2 -Wall -march=mips32r2

SDK_LIB_DIR = lib
INCLUDES = -I$(SDK_INC_DIR)

ifeq ($(TARGET),t31)
SDK_INC_DIR = include/t31
LIBS = $(SDK_LIB_DIR)/t31/uclibc/libimp.so $(SDK_LIB_DIR)/t31/uclibc/libalog.so
COMPILE_OPTS += -DPLATFORM_T31
else
SDK_INC_DIR = include/t20
LIBS = $(SDK_LIB_DIR)/t20/uclibc/libimp.so $(SDK_LIB_DIR)/t20/uclibc/libalog.so
COMPILE_OPTS += -DPLATFORM_T20
endif

LDFLAG += -Wl,-gc-sections

APP = raptor

.PHONY:all

all: 	$(APP)

$(APP): raptor.o encoder.o system.o musl_shim.o tcp.o
	$(CPLUSPLUS) $(LDFLAG) -o $@ $^ $(LIBS) -lpthread -lm -lrt
	$(STRIP) $@

%.o:%.c sample-common.h
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o *~

distclean: clean
	rm -f $(APP)
