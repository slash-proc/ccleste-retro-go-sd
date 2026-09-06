# Retro-Go SD — Celeste Classic (ccleste) GWHB homebrew
#
#   make PROJECT_KIND=homebrew
#   make host PROJECT_KIND=homebrew
#   make docker PROJECT_KIND=homebrew

#######################################
# Project identity
#######################################
PROJECT_KIND ?= homebrew

CORE_NAME  := celeste
CORE_ENTRY := app_main

CORE_C_SOURCES := \
src/main.c \
src/ccleste/celeste.c \
src/ccleste/celeste_audio.c

CORE_C_INCLUDES := \
-Isrc/ccleste

CORE_C_DEFS += -DCELESTE_SLOW_CPU

CORE_LDLIBS := -lm

# Hot .text → ITCM (LMA packed in GWHB payload; copied at boot).
CORE_LDSCRIPT := celeste.ld

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

#######################################
# SDK bridge overrides (optional)
#######################################
# (none)

#######################################
# Kind-specific compile defs + packing
#######################################
ifeq ($(PROJECT_KIND),core)
CORE_C_DEFS += \
-DPROJECT_KIND_CORE=1 \
-DCOVERFLOW=1 \
-DCHEAT_CODES=1 \
-DMAX_CHEAT_CODES=13

PACKED_BIN  := $(CORE_NAME).bin
PAD_LOGO    := src/assets/pad.png
HEADER_LOGO := src/assets/header.png

else ifeq ($(PROJECT_KIND),homebrew)
CORE_C_DEFS += \
-DPROJECT_KIND_HOMEBREW=1

PACKED_BIN := Celeste.bin
HB_NAME    := Celeste Classic
COVER_SRC  := src/assets/cover.png
COVER_JPG  := $(BUILD_DIR)/cover.jpg
COVER_WIDTH  ?= 128
COVER_HEIGHT ?= 96

else
$(error PROJECT_KIND must be 'core' or 'homebrew' (got '$(PROJECT_KIND)'))
endif

include $(GNW_CORE_SDK)/Makefile

# Default sdk objcopy omits .itcm_text; rebuild a payload that includes the
# ITCM LMA blob packed after .data (see celeste.ld).
CELESTE_PAYLOAD := $(BUILD_DIR)/$(CORE_NAME)_payload.bin

$(CELESTE_PAYLOAD): $(TARGET_ELF)
	$(V)$(ECHO) [ BIN ITCM ] $(notdir $@)
	$(V)$(CP) -O binary \
		--only-section=.core_entry \
		--only-section=.data \
		--only-section=.itcm_text \
		$< $@

PACK_CORE     := $(GNW_CORE_SDK)/tools/pack_core.py
PACK_HOMEBREW := $(GNW_CORE_SDK)/tools/pack_homebrew.py

#######################################
# Packed header version
#######################################
CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

#######################################
# Pack
#######################################
.PHONY: pack cover

.PHONY: cover
cover: $(COVER_JPG)

$(COVER_JPG): $(COVER_SRC) | $(BUILD_DIR)
	$(V)$(ECHO) "[ COVER ]" $(COVER_JPG)
	$(V)python3 -c "from pathlib import Path; from PIL import Image; \
img=Image.open('$(COVER_SRC)').convert('RGB'); \
img.thumbnail(($(COVER_WIDTH), $(COVER_HEIGHT))); \
Path('$(COVER_JPG)').parent.mkdir(parents=True, exist_ok=True); \
img.save('$(COVER_JPG)', 'JPEG', quality=85, optimize=True); \
sz=Path('$(COVER_JPG)').stat().st_size; \
assert sz <= 10*1024, f'cover too big: {sz}'; \
w,h=img.size; assert w<=186 and h<=100, (w,h); \
print(f'cover: $(COVER_JPG) ({w}x{h}, {sz} bytes)')"

pack: $(CELESTE_PAYLOAD) $(COVER_JPG)
	$(V)$(ECHO) [ PACK GWHB ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_HOMEBREW) \
		--elf $(TARGET_ELF) --bin $(CELESTE_PAYLOAD) \
		--name "$(HB_NAME)" --version "$(CORE_VERSION)" \
		--cover $(COVER_JPG) \
		--out $(PACKED_BIN)

all: pack

.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-CORE_VERSION
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-CORE_VERSION:
	@echo $(CORE_VERSION)

clean::
	$(V)rm -f $(PACKED_BIN)
ifeq ($(PROJECT_KIND),homebrew)
	$(V)rm -f $(COVER_JPG)
endif

#######################################
# Docker
#######################################
.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE) "PROJECT_KIND=$(PROJECT_KIND)"
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc) PROJECT_KIND=$(PROJECT_KIND)

docker_pull:
	$(V)$(ECHO) "[ PULL ]" $(DOCKER_IMAGE)
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash

#######################################
# Host SDL (Linux / macOS)
#######################################
include host/Makefile.host
