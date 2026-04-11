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

# xburst2 (T40/T41) toolchain uses -mfp64 ABI by default.
# Ensure largefile support matches buildroot target flags.
ifneq ($(filter T40 T41,$(PLATFORM)),)
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
endif

CFLAGS += $(EXTRA_CFLAGS)

# Build info — generate a tiny .o with string constants that each daemon links.
# Uses $(shell) so it runs at Makefile parse time, before any targets.
RSS_BUILD_HASH ?= $(shell git -C $(CURDIR) rev-parse --short HEAD 2>/dev/null || echo unknown)
RSS_BUILD_TIME ?= $(shell date -u '+%Y-%m-%dT%H:%M:%SZ')
RSS_BUILD_OBJ := $(CURDIR)/rss_build_info.o
$(shell printf 'const char *rss_build_hash = "%s";\nconst char *rss_build_time = "%s";\n' \
	'$(RSS_BUILD_HASH)' '$(RSS_BUILD_TIME)' > $(CURDIR)/rss_build_info.c)
$(RSS_BUILD_OBJ): $(CURDIR)/rss_build_info.c
	@echo "  CC      rss_build_info.c"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# Every daemon links this object for the build banner
RSS_BUILD_LIBS := $(RSS_BUILD_OBJ)

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

ifeq ($(WEBTORRENT),1)
CFLAGS += -DRAPTOR_WEBTORRENT
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

ifeq ($(TLS),1)
COMPY_CFLAGS += -DCOMPY_HAS_TLS
LDFLAGS_TLS := -lmbedtls -lmbedx509 -lmbedcrypto
endif

# Static libraries (absolute paths for sub-makes)
LIB_HAL    := $(CURDIR)/$(HAL_DIR)/libraptor_hal.a
LIB_IPC    := $(CURDIR)/$(IPC_DIR)/librss_ipc.a
LIB_COMMON := $(CURDIR)/$(COMMON_DIR)/librss_common.a
LIB_COMPY  := $(COMPY_BUILD)/libcompy.a

# TLS helper (compiled separately, only linked by daemons that need it).
# Source is in raptor-common (standalone) or sysroot (buildroot).
RSS_TLS_SRC := $(firstword $(wildcard $(CURDIR)/$(COMMON_DIR)/src/rss_tls.c) \
                           $(wildcard $(SYSROOT)/usr/share/raptor-common/rss_tls.c))
RSS_TLS_OBJ := $(CURDIR)/rss_tls.o
ifneq ($(RSS_TLS_SRC),)
$(RSS_TLS_OBJ): $(RSS_TLS_SRC)
	@echo "  CC      rss_tls.c"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@
endif

# Sysroot for finding shared libs (set by Buildroot or manually)
SYSROOT ?=
ifneq ($(SYSROOT),)
LDFLAGS_SYSROOT := -L$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/lib
else
LDFLAGS_SYSROOT :=
endif

# libc shim — link musl or uclibc shim if present in sysroot
SHIM_LIB := $(if $(wildcard $(SYSROOT)/usr/lib/libmuslshim.so $(SYSROOT)/lib/libmuslshim.so),-lmuslshim,\
             $(if $(wildcard $(SYSROOT)/usr/lib/libuclibcshim.so $(SYSROOT)/lib/libuclibcshim.so),-luclibcshim,))

# System libs for HAL-linked daemons
# SHIM_LIB must come BEFORE Ingenic SDK libs — the dynamic linker resolves
# symbols in DT_NEEDED order, so the shim's mmap must be loaded first.
LDFLAGS_HAL := $(LDFLAGS_SYSROOT) $(SHIM_LIB) -limp -lalog -lsysutils -lpersonDet_inf -ljzdl -ljzdl.m -lmxu_merge -lmxu_objdetect -lmxu_imgproc -lmxu_video -lmxu_contrib -lmxu_core -lpthread -lrt -lm -ldl -latomic -lstdc++
LDFLAGS     := $(LDFLAGS_SYSROOT) $(SHIM_LIB) -lpthread -lrt -latomic

# MIPS page size: Ingenic SoCs use 4KB pages but the toolchain defaults to
# 64KB max-page-size. Mismatched alignment causes SIGBUS on musl/uclibc.
LDFLAGS_HAL += -Wl,-z,max-page-size=0x1000
LDFLAGS     += -Wl,-z,max-page-size=0x1000

# Targets
DAEMONS := rvd rsd rad rhd rod ric rmr rmd rwd rwc
TOOLS   := raptorctl ringdump rac rlatency

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

rvd: $(LIB_HAL) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rvd"
	$(Q)$(MAKE) -C rvd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS_HAL)" Q="$(Q)"

rsd: $(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rsd"
	$(Q)$(MAKE) -C rsd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS)" Q="$(Q)"

rad: $(LIB_HAL) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rad"
	$(Q)$(MAKE) -C rad CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS_HAL) $(LDFLAGS_AAC_ENC) $(LDFLAGS_OPUS)" Q="$(Q)"

rhd: $(LIB_IPC) $(LIB_COMMON) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rhd"
	$(Q)$(MAKE) -C rhd CC="$(CC)" CFLAGS="$(CFLAGS) -DRSS_HAS_TLS" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS)" Q="$(Q)"

rod: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rod"
	$(Q)$(MAKE) -C rod CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) -lschrift $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ric: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   ric"
	$(Q)$(MAKE) -C ric CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) -ldl" Q="$(Q)"

rmr: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rmr"
	$(Q)$(MAKE) -C rmr CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rmd: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rmd"
	$(Q)$(MAKE) -C rmd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rwd: $(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rwd"
	$(Q)$(MAKE) -C rwd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS) -DMBEDTLS_ALLOW_PRIVATE_ACCESS -DRSS_HAS_TLS" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS) -lopus $(LDFLAGS_AAC_DEC)" WEBTORRENT=$(WEBTORRENT) Q="$(Q)"

rwc: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rwc"
	$(Q)$(MAKE) -C rwc CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

# -- Tools --

raptorctl: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   raptorctl"
	$(Q)$(MAKE) -C raptorctl CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ringdump: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   ringdump"
	$(Q)$(MAKE) -C ringdump CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rac: $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rac"
	$(Q)$(MAKE) -C rac CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_MP3) $(LDFLAGS_AAC_DEC) $(LDFLAGS_OPUS)" Q="$(Q)"

rlatency:
	@echo "  BUILD   rlatency"
	$(Q)$(MAKE) -C rlatency CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

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
	rm -f rss_build_info.c rss_build_info.o rss_tls.o
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
