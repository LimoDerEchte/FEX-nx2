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
	$(RM) $(BUILD)/dynamic-start.o $(BUILD)/dynamic-main.o $(BUILD)/dynamic-loader.o $(BUILD)/dynamic-library.o
	$(RM) $(DYNAMIC_DIR)/games/dynamic-demo.elf $(DYNAMIC_DIR)/rootfs/lib/ld-fex-demo-x86-64.so.2 $(DYNAMIC_DIR)/rootfs/lib/libfex-demo.so

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

DYNAMIC_DIR := $(BUILD)/dynamic-demo
DYNAMIC_CFLAGS := -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror

.PHONY: dynamic-demo
dynamic-demo: $(DYNAMIC_DIR)/games/dynamic-demo.elf $(DYNAMIC_DIR)/rootfs/lib/ld-fex-demo-x86-64.so.2

$(BUILD)/dynamic-start.o: Dynamic/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/dynamic-main.o: Dynamic/Demo.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/dynamic-loader.o: Dynamic/Loader.c SDL2/GuestSyscall.h | $(BUILD)
	$(HOST_X86_CC) $(DYNAMIC_CFLAGS) -c $< -o $@

$(BUILD)/dynamic-library.o: Dynamic/Library.c SDL2/GuestSyscall.h | $(BUILD)
	$(HOST_X86_CC) $(DYNAMIC_CFLAGS) -fPIC -c $< -o $@

$(DYNAMIC_DIR)/rootfs/lib/ld-fex-demo-x86-64.so.2: $(BUILD)/dynamic-start.o $(BUILD)/dynamic-loader.o HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T HelloGuest.ld $(filter %.o,$^) -o $@

$(DYNAMIC_DIR)/rootfs/lib/libfex-demo.so: $(BUILD)/dynamic-library.o
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -shared --hash-style=sysv -soname libfex-demo.so $< -o $@

$(DYNAMIC_DIR)/games/dynamic-demo.elf: $(BUILD)/dynamic-main.o $(DYNAMIC_DIR)/rootfs/lib/libfex-demo.so
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --hash-style=sysv --dynamic-linker /lib/ld-fex-demo-x86-64.so.2 $^ -o $@

HOST_CXX ?= c++
STOCK_TRUE ?= /usr/bin/true
STOCK_DIR ?= $(BUILD)/stock-glibc
STOCK_LIB_DIR ?=
STOCK_CRT_DIR ?=
STOCK_PROBE := $(BUILD)/glibc-probe.elf
ifneq ($(strip $(STOCK_CRT_DIR)),)
STOCK_PROBE := $(BUILD)/glibc-probe-baseline.elf
endif

.PHONY: stock-glibc check-guest-memory
stock-glibc: $(STOCK_PROBE)
	python3 LinuxRuntime/StageRootFS.py $(STOCK_DIR)/fex "$(STOCK_TRUE)" "$<" "$(HOST_X86_CC)" "$(STOCK_LIB_DIR)"

$(BUILD)/glibc-probe.elf: LinuxRuntime/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -O2 -fPIE -pie -Wall -Wextra -Werror $< -ldl -o $@

$(BUILD)/glibc-probe-baseline.o: LinuxRuntime/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/glibc-probe-baseline.elf: $(BUILD)/glibc-probe-baseline.o $(STOCK_CRT_DIR)/Scrt1.o $(STOCK_CRT_DIR)/crti.o $(STOCK_CRT_DIR)/crtn.o $(STOCK_LIB_DIR)/libc.so.6
	$(HOST_X86_LD) -m elf_x86_64 -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 -rpath-link "$(STOCK_LIB_DIR)" \
		"$(STOCK_CRT_DIR)/Scrt1.o" "$(STOCK_CRT_DIR)/crti.o" $< "$(STOCK_LIB_DIR)/libc.so.6" "$(STOCK_CRT_DIR)/crtn.o" -o $@

check-guest-memory: $(BUILD)/guest-address-space-test
	$<

$(BUILD)/guest-address-space-test: LinuxRuntime/AddressSpaceTest.cpp GuestAddressSpace.h GuestElf.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

DEBIAN_ROOT ?=
DEBIAN_TEST_DIR ?= $(BUILD)/debian-runtime
DEBIAN_LIBS = $(DEBIAN_ROOT)/lib/x86_64-linux-gnu
DEBIAN_CRT = $(DEBIAN_ROOT)/usr/lib/x86_64-linux-gnu

.PHONY: debian-runtime
debian-runtime: $(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf
	python3 LinuxRuntime/StageDebian.py "$(DEBIAN_ROOT)" "$(DEBIAN_TEST_DIR)" $^

$(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf: $(BUILD)/%-debian.elf: $(BUILD)/%.o $(DEBIAN_CRT)/Scrt1.o $(DEBIAN_CRT)/crti.o $(DEBIAN_CRT)/crtn.o $(DEBIAN_LIBS)/libc.so.6
	$(HOST_X86_LD) -m elf_x86_64 -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 -rpath-link "$(DEBIAN_LIBS)" \
		"$(DEBIAN_CRT)/Scrt1.o" "$(DEBIAN_CRT)/crti.o" $< "$(DEBIAN_LIBS)/libc.so.6" "$(DEBIAN_CRT)/crtn.o" -o $@

$(BUILD)/thread-ipc.o: LinuxRuntime/ThreadIpcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/glibc-probe.o: LinuxRuntime/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

.PHONY: check-guest-log
check-guest-log: $(BUILD)/guest-log-buffer-test
	$<

$(BUILD)/guest-log-buffer-test: LinuxRuntime/GuestLogBufferTest.cpp GuestLogBuffer.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@
