# Neo Geo Pocket / Color (RACE) — standalone dynamic core for Game & Watch Retro-Go SD.
#
#   make                  — build + pack → race.bin
#   make host             — Linux/macOS SDL binary → race_host
#   make host HOST_SDL=3  — same with SDL3
#   make docker           — same build inside Docker (no host toolchain)
#
# Memory: hot TLCS/CZ80/graphics .text in ITCM; FB + palette + DAC in DTCM;
# mainram/cpurom in RAM_EMU. ITCM is not used for heap data.
#
# Verbose compiler lines: make V=

#######################################
# Project identity
#######################################
PROJECT_KIND ?= core

CORE_NAME  := race
CORE_ENTRY := app_main

CORE_RACE := src/race

CORE_C_SOURCES := \
$(CORE_RACE)/cz80.c \
$(CORE_RACE)/cz80_support.c \
$(CORE_RACE)/flash.c \
$(CORE_RACE)/graphics.c \
$(CORE_RACE)/race_main.c \
$(CORE_RACE)/neopop_blip.c \
$(CORE_RACE)/neopopsound.c \
$(CORE_RACE)/ngpBios.c \
$(CORE_RACE)/race-memory.c \
$(CORE_RACE)/sound.c \
$(CORE_RACE)/state.c \
$(CORE_RACE)/tlcs900h.c \
$(CORE_RACE)/deps/blip/Blip_Buffer.c \
$(CORE_RACE)/gnw_ldiv.c \
src/main.c

CORE_C_INCLUDES := \
-I$(CORE_RACE) \
-I$(CORE_RACE)/include \
-I$(CORE_RACE)/deps/blip \
-Isrc

CORE_C_DEFS := \
-DPROJECT_KIND_CORE=1 \
-DCOVERFLOW=1 \
-DCHEAT_CODES=0 \
-DCZ80 \
-DGNW_NGP \
-D_MAX_PATH=260

# Hot interpreter / renderer / Z80 in ITCM (see race_core.ld).
CORE_LDSCRIPT := race_core.ld
CORE_EXTRA_SEGMENTS := itcm:core_itcm

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

#######################################
# Kind-specific packing
#######################################
ifeq ($(PROJECT_KIND),core)
PACKED_BIN  := race.bin
PAD_LOGO    := src/assets/pad.bmp
HEADER_LOGO := src/assets/header.bmp
else
$(error This project is a dynamic core only (PROJECT_KIND=core))
endif

include $(GNW_CORE_SDK)/Makefile

# CZ80 jump tables inflate Thumb size; match firmware recipe.
$(BUILD_DIR)/cz80.o: CFLAGS += -fno-jump-tables

PACK_CORE := $(GNW_CORE_SDK)/tools/pack_core.py

CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

.PHONY: pack

pack: $(TARGET_BIN) $(PAD_LOGO) $(HEADER_LOGO)
	$(V)$(ECHO) [ PACK CORE ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_CORE) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--system name="Neo Geo Pocket",dirname=ngp,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_LOGO),ext="ngp ngc ngpc",parse=rom \
		--logo-invert \
		--core-name "RACE" \
		--version "$(CORE_VERSION)" \
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
