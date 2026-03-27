# Raptor Streaming System -- top-level build
#
# Usage:
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- rvd ringdump
#   make clean
#
# Required:
#   PLATFORM       - Target SoC: T20, T21, T23, T30, T31, T32, T40, T41
#   CROSS_COMPILE  - Cross-compiler prefix

ifeq ($(filter clean distclean build,$(MAKECMDGOALS)),)
ifndef PLATFORM
$(error PLATFORM not set. Use: make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-)
endif
endif

# Sibling repos
HAL_DIR    := ../raptor-hal
IPC_DIR    := ../raptor-ipc
COMMON_DIR := ../raptor-common
COMPY_DIR  := ../compy

# Toolchain
CC     := $(CROSS_COMPILE)gcc
AR     := $(CROSS_COMPILE)ar
STRIP  := $(CROSS_COMPILE)strip

# Common flags for all daemons
EXTRA_CFLAGS ?=
CFLAGS := -Wall -Wextra -Werror=implicit-function-declaration
CFLAGS += -std=gnu11 -D_GNU_SOURCE
CFLAGS += -DPLATFORM_$(PLATFORM)
CFLAGS += -I$(CURDIR)/$(HAL_DIR)/include
CFLAGS += -I$(CURDIR)/$(IPC_DIR)/include
CFLAGS += -I$(CURDIR)/$(COMMON_DIR)/include
CFLAGS += $(EXTRA_CFLAGS)

ifeq ($(DEBUG),1)
CFLAGS += -O0 -g
else
CFLAGS += -Os
endif

ifeq ($(AUDIO_EFFECTS),1)
CFLAGS += -DRAPTOR_AUDIO_EFFECTS
endif

ifeq ($(AAC),1)
CFLAGS += -DRAPTOR_AAC
LDFLAGS_AAC_ENC := -lfaac
LDFLAGS_AAC_DEC := -lhelix-aac
endif

ifeq ($(MP3),1)
CFLAGS += -DRAPTOR_MP3 -DARDUINO
LDFLAGS_MP3 := -lhelix-mp3
endif

ifeq ($(OPUS),1)
CFLAGS += -DRAPTOR_OPUS
LDFLAGS_OPUS := -lopus
endif

ifeq ($(V),1)
Q :=
else
Q := @
endif

# Compy include paths (for RSD only)
COMPY_BUILD  := $(CURDIR)/$(COMPY_DIR)/build-mips
COMPY_CFLAGS := -I$(CURDIR)/$(COMPY_DIR)/include \
                -I$(COMPY_BUILD)/_deps/slice99-src \
                -I$(COMPY_BUILD)/_deps/datatype99-src \
                -I$(COMPY_BUILD)/_deps/interface99-src \
                -I$(COMPY_BUILD)/_deps/metalang99-src/include

# TLS support — if mbedTLS is in sysroot, compy was built with COMPY_HAS_TLS
COMPY_TLS_CFLAGS := $(if $(wildcard $(SYSROOT)/usr/lib/libmbedtls.so $(SYSROOT)/lib/libmbedtls.so),-DCOMPY_HAS_TLS,)
COMPY_CFLAGS += $(COMPY_TLS_CFLAGS)

# Static libraries (absolute paths for sub-makes)
LIB_HAL    := $(CURDIR)/$(HAL_DIR)/libraptor_hal.a
LIB_IPC    := $(CURDIR)/$(IPC_DIR)/librss_ipc.a
LIB_COMMON := $(CURDIR)/$(COMMON_DIR)/librss_common.a
LIB_COMPY  := $(COMPY_BUILD)/libcompy.a

# Sysroot for finding shared libs (set by Buildroot or manually)
SYSROOT ?=
ifneq ($(SYSROOT),)
LDFLAGS_SYSROOT := -L$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/lib
else
LDFLAGS_SYSROOT :=
endif

# uclibc shim — only link if present in sysroot
SHIM_LIB := $(if $(wildcard $(SYSROOT)/usr/lib/libuclibcshim.so $(SYSROOT)/lib/libuclibcshim.so),-luclibcshim,)

# System libs for HAL-linked daemons
LDFLAGS_HAL := $(LDFLAGS_SYSROOT) -limp -lalog -lsysutils $(SHIM_LIB) -lpthread -lrt -lm -ldl -latomic
LDFLAGS     := $(LDFLAGS_SYSROOT) $(SHIM_LIB) -lpthread -lrt -latomic

# Targets
DAEMONS := rvd rsd rad rhd rod ric rmr
TOOLS   := raptorctl ringdump rac

.PHONY: all clean libs $(DAEMONS) $(TOOLS) install

all: libs $(DAEMONS) $(TOOLS)

# Phase 1 quick target
phase1: libs rvd ringdump raptorctl

# -- Libraries --

libs: $(LIB_HAL) $(LIB_IPC) $(LIB_COMMON)

$(LIB_HAL):
	@echo "  BUILD   raptor-hal"
	$(Q)$(MAKE) -C $(HAL_DIR) PLATFORM=$(PLATFORM) CROSS_COMPILE=$(CROSS_COMPILE) \
		$(if $(DEBUG),DEBUG=1,)

$(LIB_IPC):
	@echo "  BUILD   raptor-ipc"
	$(Q)$(MAKE) -C $(IPC_DIR) CC="$(CC)" AR="$(AR)"

$(LIB_COMMON):
	@echo "  BUILD   raptor-common"
	$(Q)$(MAKE) -C $(COMMON_DIR) CC="$(CC)" AR="$(AR)"

# -- Daemons --

rvd: $(LIB_HAL) $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   rvd"
	$(Q)$(MAKE) -C rvd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL) $(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS_HAL)" Q="$(Q)"

# mbedTLS — link if present in sysroot (enables RTSPS via compy COMPY_HAS_TLS)
MBEDTLS_LIB := $(if $(wildcard $(SYSROOT)/usr/lib/libmbedtls.so $(SYSROOT)/lib/libmbedtls.so),-lmbedtls -lmbedx509 -lmbedcrypto,)

rsd: $(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY)
	@echo "  BUILD   rsd"
	$(Q)$(MAKE) -C rsd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY)" \
		LDFLAGS="$(LDFLAGS) $(MBEDTLS_LIB)" Q="$(Q)"

rad: $(LIB_HAL) $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   rad"
	$(Q)$(MAKE) -C rad CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL) $(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS_HAL) $(LDFLAGS_AAC_ENC) $(LDFLAGS_OPUS)" Q="$(Q)"

rhd: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   rhd"
	$(Q)$(MAKE) -C rhd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rod: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   rod"
	$(Q)$(MAKE) -C rod CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) -lschrift" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ric: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   ric"
	$(Q)$(MAKE) -C ric CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rmr: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   rmr"
	$(Q)$(MAKE) -C rmr CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

# -- Tools --

raptorctl: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   raptorctl"
	$(Q)$(MAKE) -C raptorctl CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ringdump: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   ringdump"
	$(Q)$(MAKE) -C ringdump CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rac: $(LIB_IPC) $(LIB_COMMON)
	@echo "  BUILD   rac"
	$(Q)$(MAKE) -C rac CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_MP3) $(LDFLAGS_AAC_DEC) $(LDFLAGS_OPUS)" Q="$(Q)"

# -- Collect binaries --

build:
	@mkdir -p build
	@for d in $(DAEMONS) $(TOOLS); do \
		if [ -f $$d/$$d ]; then cp $$d/$$d build/; fi; \
	done
	@echo "  Binaries collected in build/"

# -- Clean --

clean:
	@for d in $(DAEMONS) $(TOOLS); do \
		echo "  CLEAN   $$d"; \
		$(MAKE) -C $$d clean 2>/dev/null || true; \
	done
	rm -rf build

distclean: clean
	$(MAKE) -C $(HAL_DIR) clean
	$(MAKE) -C $(IPC_DIR) clean 2>/dev/null || true
	$(MAKE) -C $(COMMON_DIR) clean 2>/dev/null || true

# -- Install --

install:
	install -d $(DESTDIR)/usr/bin
	install -d $(DESTDIR)/etc
	install -d $(DESTDIR)/etc/init.d
	for d in $(DAEMONS); do \
		[ -f $$d/$$d ] && install -m 0755 $$d/$$d $(DESTDIR)/usr/bin/ || true; \
	done
	for t in $(TOOLS); do \
		[ -f $$t/$$t ] && install -m 0755 $$t/$$t $(DESTDIR)/usr/bin/ || true; \
	done
	install -m 0644 config/raptor.conf $(DESTDIR)/etc/raptor.conf
	install -m 0755 config/S31raptor $(DESTDIR)/etc/init.d/S31raptor
