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
	$(RM) $(BUILD)/maze-start.o $(BUILD)/maze.o $(MAZE_DIR)/sd-maze.elf $(MAZE_DIR)/sd-maze.elf.args $(MAZE_DIR)/data/maze.txt
	$(RM) $(BUILD)/texture-start.o $(BUILD)/texture-demo.o $(GRAPHICS_DIR)/texture-demo.elf $(GRAPHICS_DIR)/data/sprite.rgba
	$(RM) $(BUILD)/sdl2-start.o $(BUILD)/sdl2-guest.o $(BUILD)/star-catch.o $(STAR_DIR)/star-catch.elf $(STAR_DIR)/data/star.rgba

-include $(OFILES:.o=.d)

HOST_X86_AS ?= as
HOST_X86_LD ?= ld

.PHONY: hello-elf
hello-elf: $(BUILD)/fex-hello.elf

$(BUILD)/fex-hello.o: HelloGuest.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/fex-hello.elf: $(BUILD)/fex-hello.o HelloGuest.ld
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T HelloGuest.ld $< -o $@

HOST_X86_CC ?= cc
# Use a hyphenated folder so the same targets work with make and on the SD card.
MAZE_DIR := $(BUILD)/sd-maze

.PHONY: maze-elf
maze-elf: $(MAZE_DIR)/sd-maze.elf $(MAZE_DIR)/sd-maze.elf.args $(MAZE_DIR)/data/maze.txt

$(BUILD)/maze-start.o: Maze/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/maze.o: Maze/Maze.c | $(BUILD)
	$(HOST_X86_CC) -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(MAZE_DIR)/sd-maze.elf: $(BUILD)/maze-start.o $(BUILD)/maze.o HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T HelloGuest.ld $(filter %.o,$^) -o $@

$(MAZE_DIR)/sd-maze.elf.args: Maze/sd-maze.elf.args
	mkdir -p $(@D)
	cp $< $@

$(MAZE_DIR)/data/maze.txt: Maze/data/maze.txt
	mkdir -p $(@D)
	cp $< $@

GRAPHICS_DIR := $(BUILD)/texture-demo

.PHONY: texture-demo
texture-demo: $(GRAPHICS_DIR)/texture-demo.elf $(GRAPHICS_DIR)/data/sprite.rgba

$(BUILD)/texture-start.o: Graphics/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/texture-demo.o: Graphics/TextureDemo.c GuestVideo.h | $(BUILD)
	$(HOST_X86_CC) -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(GRAPHICS_DIR)/texture-demo.elf: $(BUILD)/texture-start.o $(BUILD)/texture-demo.o HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T HelloGuest.ld $(filter %.o,$^) -o $@

$(GRAPHICS_DIR)/data/sprite.rgba: Graphics/MakeTexture.py
	mkdir -p $(@D)
	python3 $< $@

SDL2_CFLAGS ?= $(shell pkg-config --cflags sdl2)
STAR_DIR := $(BUILD)/star-catch

.PHONY: star-catch
star-catch: $(STAR_DIR)/star-catch.elf $(STAR_DIR)/data/star.rgba

$(BUILD)/sdl2-start.o: SDL2/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/sdl2-guest.o: SDL2/SDL2.c SDL2/GuestSyscall.h GuestVideo.h | $(BUILD)
	$(HOST_X86_CC) $(SDL2_CFLAGS) -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/star-catch.o: SDL2/StarCatch.c | $(BUILD)
	$(HOST_X86_CC) $(SDL2_CFLAGS) -DSDL_MAIN_HANDLED -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(STAR_DIR)/star-catch.elf: $(BUILD)/sdl2-start.o $(BUILD)/sdl2-guest.o $(BUILD)/star-catch.o HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T HelloGuest.ld $(filter %.o,$^) -o $@

$(STAR_DIR)/data/star.rgba: Graphics/MakeTexture.py
	mkdir -p $(@D)
	python3 $< $@
