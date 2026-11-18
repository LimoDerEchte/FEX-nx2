# SPDX-License-Identifier: MIT
DEVKITPRO ?= /opt/devkitpro
export DEVKITPRO
include $(DEVKITPRO)/libounce/switch2_rules

ROOT := $(abspath ../..)
BUILD := build
TARGET := $(BUILD)/fex-switch2-jit
FMT_INCLUDE ?= $(ROOT)/External/fmt/include

# Isolate native fmt headers from the rest of the host's include tree.
ifeq ($(wildcard $(FMT_INCLUDE)/fmt/format.h),)
HOST_FMT_INCLUDE := $(shell pkg-config --variable=includedir fmt 2>/dev/null)
ifneq ($(wildcard $(HOST_FMT_INCLUDE)/fmt/format.h),)
FMT_INCLUDE := $(abspath $(BUILD)/headers)
FMT_HEADERS := $(FMT_INCLUDE)/fmt
endif
endif

ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CPPFLAGS += -isystem $(LIBNX)/include -I$(ROOT)/CodeEmitter -I$(ROOT)/FEXCore/include -I$(ROOT)/FEXHeaderUtils -isystem $(FMT_INCLUDE)
CXXFLAGS += $(ARCH) -std=gnu++20 -O2 -Wall -Wextra -fno-exceptions -ffunction-sections -fdata-sections
LDFLAGS += $(ARCH) -specs=$(LIBNX)/switch.specs -Wl,--gc-sections
LD := $(CXX)
LIBPATHS := -L$(LIBNX)/lib
LIBS := -lkaihen -lnx
OFILES := $(BUILD)/JitProbe.o
DEPSDIR := $(BUILD)
APP_TITLE := FEX Switch 2 JIT Probe
APP_AUTHOR := FEX Switch 2 fork

.PHONY: all clean
all: $(TARGET).nro

ifneq ($(FMT_HEADERS),)
$(FMT_HEADERS): | $(BUILD)
	mkdir -p $(@D)
	ln -sfn $(HOST_FMT_INCLUDE)/fmt $@
endif

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.cpp Makefile | $(BUILD) $(FMT_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TARGET).elf: $(OFILES) Makefile
	$(LD) $(LDFLAGS) $(OFILES) $(OVERLAY_LIBPATHS) $(LIBPATHS) $(LIBS) -o $@
$(TARGET).nacp: | $(BUILD)
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp $(APP_ICON)
$(TARGET).nro: NROFLAGS = --icon=$(APP_ICON) --nacp=$(TARGET).nacp

clean:
	$(RM) $(BUILD)/JitProbe.o $(BUILD)/JitProbe.d $(TARGET).elf $(TARGET).nacp $(TARGET).nro $(FMT_HEADERS) $(BUILD)/fex-hello.o $(BUILD)/fex-hello.elf

-include $(OFILES:.o=.d)

HOST_X86_AS ?= as
HOST_X86_LD ?= ld

.PHONY: hello-elf
hello-elf: $(BUILD)/fex-hello.elf

$(BUILD)/fex-hello.o: HelloGuest.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/fex-hello.elf: $(BUILD)/fex-hello.o HelloGuest.ld
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T HelloGuest.ld $< -o $@
