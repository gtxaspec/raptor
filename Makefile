# Raptor Streaming System -- top-level build
#
# Usage:
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- rvd ringdump
#   make clean
#
# Required:
#   PLATFORM       - Target SoC: T20, T21, T23, T30, T31, T32, T33, T40, T41, A1
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
CFLAGS += -ffunction-sections -fdata-sections -flto
CFLAGS += -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident
CFLAGS += -DPLATFORM_$(PLATFORM)
CFLAGS += -I$(CURDIR)/$(HAL_DIR)/include
CFLAGS += -I$(CURDIR)/$(IPC_DIR)/include
CFLAGS += -I$(CURDIR)/$(COMMON_DIR)/include

# xburst2 (T40/T41/A1) toolchain uses -mfp64 ABI by default.
# Ensure largefile support matches buildroot target flags.
ifneq ($(filter T40 T41 A1,$(PLATFORM)),)
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
endif

CFLAGS += $(EXTRA_CFLAGS)

# Build info — generate a tiny .o with string constants that each daemon links.
# Uses $(shell) so it runs at Makefile parse time, before any targets.
RSS_BUILD_HASH ?= $(shell git -C $(CURDIR) rev-parse --short HEAD 2>/dev/null || echo unknown)
RSS_BUILD_TIME ?= $(shell date -u '+%Y-%m-%dT%H:%M:%SZ')
RSS_BUILD_OBJ := $(CURDIR)/rss_build_info.o
$(shell printf 'const char *rss_build_hash = "%s";\nconst char *rss_build_time = "%s";\nconst char *rss_build_platform = "%s";\n' \
	'$(RSS_BUILD_HASH)' '$(RSS_BUILD_TIME)' '$(PLATFORM)' > $(CURDIR)/rss_build_info.c)
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

# Library file paths (for Make dependencies and build triggers)
LIB_HAL_VIDEO_FILE ?= $(CURDIR)/$(HAL_DIR)/libraptor_hal_video.a
LIB_HAL_AUDIO_FILE ?= $(CURDIR)/$(HAL_DIR)/libraptor_hal_audio.a
LIB_IPC_FILE    ?= $(CURDIR)/$(IPC_DIR)/librss_ipc.so
LIB_COMMON_FILE ?= $(CURDIR)/$(COMMON_DIR)/librss_common.so
LIB_COMPY_FILE  ?= $(COMPY_BUILD)/libcompy.a

# Library link flags (for linker command line)
LIB_HAL_VIDEO ?= $(LIB_HAL_VIDEO_FILE)
LIB_HAL_AUDIO ?= $(LIB_HAL_AUDIO_FILE)
LIB_IPC    ?= -L$(CURDIR)/$(IPC_DIR) -lrss_ipc
LIB_COMMON ?= -L$(CURDIR)/$(COMMON_DIR) -lrss_common
LIB_COMPY  ?= $(LIB_COMPY_FILE)

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

# libc shim — prefer static archive (eliminates .so from device), fall back to shared
SHIM_A := $(firstword $(wildcard $(SYSROOT)/usr/lib/libmuslshim.a $(SYSROOT)/lib/libmuslshim.a \
                                 $(SYSROOT)/usr/lib/libuclibcshim.a $(SYSROOT)/lib/libuclibcshim.a))
ifneq ($(SHIM_A),)
# Static link: pull all symbols (libimp.so resolves them from the executable)
SHIM_LIB := -Wl,--whole-archive $(SHIM_A) -Wl,--no-whole-archive -Wl,--export-dynamic
else
SHIM_LIB := $(if $(wildcard $(SYSROOT)/usr/lib/libmuslshim.so $(SYSROOT)/lib/libmuslshim.so),-lmuslshim,\
             $(if $(wildcard $(SYSROOT)/usr/lib/libuclibcshim.so $(SYSROOT)/lib/libuclibcshim.so),-luclibcshim,))
endif

# System libs for HAL-linked daemons
# Shim must come BEFORE Ingenic SDK libs — symbols must be resolved first.
LDFLAGS_HAL := $(LDFLAGS_SYSROOT) $(SHIM_LIB) -limp -lalog -lpthread -lrt -lm -ldl -latomic

# IVS detection libs — optional, no MXU needed (statically linked in .so)
ifeq ($(IVS_DETECT),1)
CFLAGS += -DIVS_DETECT
LDFLAGS_HAL += -ljzdl.m -lstdc++
ifeq ($(PERSONDET),1)
CFLAGS += -DPERSONDET
LDFLAGS_HAL += -lpersonDet_inf -ljzdl
endif
endif
LDFLAGS     := $(LDFLAGS_SYSROOT) -lpthread -lrt -latomic

# MIPS page size: Ingenic SoCs use 4KB pages but the toolchain defaults to
# 64KB max-page-size. Mismatched alignment causes SIGBUS on musl/uclibc.
LDFLAGS_HAL += -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--as-needed -Wl,-rpath,/usr/lib -flto
LDFLAGS     += -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--as-needed -Wl,-rpath,/usr/lib -flto
# rpath-link for local builds (finding .so at link time)
LDFLAGS_HAL += -Wl,-rpath-link,$(CURDIR)/$(IPC_DIR) -Wl,-rpath-link,$(CURDIR)/$(COMMON_DIR)
LDFLAGS     += -Wl,-rpath-link,$(CURDIR)/$(IPC_DIR) -Wl,-rpath-link,$(CURDIR)/$(COMMON_DIR)

# Targets
DAEMONS := rvd rsd rad rhd rod ric rmr rmd rwd rwc rfs
TOOLS   := raptorctl ringdump rac rlatency

.PHONY: all clean libs $(DAEMONS) $(TOOLS) install

all: libs $(DAEMONS) $(TOOLS)

# Phase 1 quick target
phase1: libs rvd ringdump raptorctl

# -- Libraries --

libs: $(LIB_HAL_VIDEO_FILE) $(LIB_HAL_AUDIO_FILE) $(LIB_IPC_FILE) $(LIB_COMMON_FILE)

$(LIB_HAL_VIDEO_FILE) $(LIB_HAL_AUDIO_FILE):
	@echo "  BUILD   raptor-hal"
	$(Q)$(MAKE) -C $(HAL_DIR) PLATFORM=$(PLATFORM) CROSS_COMPILE=$(CROSS_COMPILE) \
		$(if $(DEBUG),DEBUG=1,)

$(LIB_IPC_FILE):
	@echo "  BUILD   raptor-ipc"
	$(Q)$(MAKE) -C $(IPC_DIR) CC="$(CC)"

$(LIB_COMMON_FILE):
	@echo "  BUILD   raptor-common"
	$(Q)$(MAKE) -C $(COMMON_DIR) CC="$(CC)"

# -- Daemons --

rvd: $(LIB_HAL_VIDEO_FILE) $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rvd"
	$(Q)$(MAKE) -C rvd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL_VIDEO) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS_HAL)" Q="$(Q)"

rsd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(LIB_COMPY_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rsd"
	$(Q)$(MAKE) -C rsd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS)" Q="$(Q)"

rad: $(LIB_HAL_AUDIO_FILE) $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rad"
	$(Q)$(MAKE) -C rad CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL_AUDIO) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS_HAL) $(LDFLAGS_AAC_ENC) $(LDFLAGS_OPUS)" Q="$(Q)"

rhd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rhd"
	$(Q)$(MAKE) -C rhd CC="$(CC)" CFLAGS="$(CFLAGS) -DRSS_HAS_TLS" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS)" Q="$(Q)"

rod: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rod"
	$(Q)$(MAKE) -C rod CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) -lschrift $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ric: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   ric"
	$(Q)$(MAKE) -C ric CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) -ldl" Q="$(Q)"

rmr: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rmr"
	$(Q)$(MAKE) -C rmr CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rmd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rmd"
	$(Q)$(MAKE) -C rmd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rwd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(LIB_COMPY_FILE) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rwd"
	$(Q)$(MAKE) -C rwd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS) -DMBEDTLS_ALLOW_PRIVATE_ACCESS -DRSS_HAS_TLS" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS) -lopus $(LDFLAGS_AAC_DEC)" WEBTORRENT=$(WEBTORRENT) Q="$(Q)"

rwc: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rwc"
	$(Q)$(MAKE) -C rwc CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

LIBMOV_DIR := $(CURDIR)/.deps/media-server/libmov

rfs: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rfs"
	$(Q)$(MAKE) -C rfs CC="$(CC)" \
		CFLAGS="$(CFLAGS) -I$(CURDIR)/rad -I$(LIBMOV_DIR)/include -I$(LIBMOV_DIR)/source" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_AAC_ENC) $(LDFLAGS_OPUS) $(LDFLAGS_MP3) $(LDFLAGS_AAC_DEC)" \
		RAD_DIR="$(CURDIR)/rad" LIBMOV_DIR="$(LIBMOV_DIR)" Q="$(Q)"

# -- Tools --

raptorctl: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   raptorctl"
	$(Q)$(MAKE) -C raptorctl CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ringdump: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   ringdump"
	$(Q)$(MAKE) -C ringdump CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rac: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
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
