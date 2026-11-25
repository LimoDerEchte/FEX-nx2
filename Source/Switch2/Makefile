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

# Milestone 3 SDK probe: memory, faults, threads and sockets on the console.
M3PROBE := $(BUILD)/fex-switch2-m3probe
.PHONY: m3-probe
m3-probe: $(M3PROBE).nro
$(M3PROBE).elf: $(BUILD)/Milestone3Probe.o Makefile
	$(LD) $(LDFLAGS) $(BUILD)/Milestone3Probe.o $(OVERLAY_LIBPATHS) $(LIBPATHS) -lnx -o $@
$(M3PROBE).nacp: APP_TITLE := FEX Switch 2 M3 Probe
$(M3PROBE).nacp: | $(BUILD)
$(M3PROBE).nro: $(M3PROBE).elf $(M3PROBE).nacp $(APP_ICON)
$(M3PROBE).nro: NROFLAGS = --icon=$(APP_ICON) --nacp=$(M3PROBE).nacp

# Wildcard-bind probe: does Horizon forbid bind(0.0.0.0)? Log: sdmc:/switch/fex-bindprobe.log
BINDPROBE := $(BUILD)/fex-switch2-bindprobe
.PHONY: bind-probe
bind-probe: $(BINDPROBE).nro
$(BINDPROBE).elf: $(BUILD)/BindProbe.o Makefile
	$(LD) $(LDFLAGS) $(BUILD)/BindProbe.o $(OVERLAY_LIBPATHS) $(LIBPATHS) -lnx -o $@
$(BINDPROBE).nacp: APP_TITLE := FEX Switch 2 Bind Probe
$(BINDPROBE).nacp: | $(BUILD)
$(BINDPROBE).nro: $(BINDPROBE).elf $(BINDPROBE).nacp $(APP_ICON)
$(BINDPROBE).nro: NROFLAGS = --icon=$(APP_ICON) --nacp=$(BINDPROBE).nacp

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
	$(RM) $(BUILD)/Milestone3Probe.o $(BUILD)/Milestone3Probe.d $(M3PROBE).elf $(M3PROBE).nacp $(M3PROBE).nro
	$(RM) $(BUILD)/BindProbe.o $(BUILD)/BindProbe.d $(BINDPROBE).elf $(BINDPROBE).nacp $(BINDPROBE).nro
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

.PHONY: stock-glibc check-guest-memory check-guest-arena
stock-glibc: $(STOCK_PROBE)
	python3 LinuxRuntime/StageRootFS.py $(STOCK_DIR)/fex "$(STOCK_TRUE)" "$<" "$(HOST_X86_CC)" "$(STOCK_LIB_DIR)"

$(BUILD)/glibc-probe.elf: LinuxRuntime/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -O2 -fPIE -pie -Wall -Wextra -Werror $< -ldl -o $@

$(BUILD)/glibc-probe-baseline.o: LinuxRuntime/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/glibc-probe-baseline.elf: $(BUILD)/glibc-probe-baseline.o $(STOCK_CRT_DIR)/Scrt1.o $(STOCK_CRT_DIR)/crti.o $(STOCK_CRT_DIR)/crtn.o $(STOCK_LIB_DIR)/libc.so.6
	$(HOST_X86_LD) -m elf_x86_64 -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 -rpath-link "$(STOCK_LIB_DIR)" \
		"$(STOCK_CRT_DIR)/Scrt1.o" "$(STOCK_CRT_DIR)/crti.o" $< "$(STOCK_LIB_DIR)/libc.so.6" "$(STOCK_CRT_DIR)/crtn.o" -o $@

check-guest-arena: $(BUILD)/guest-arena-test
	$<

$(BUILD)/guest-arena-test: LinuxRuntime/GuestArenaTest.cpp GuestArena.h GuestElf.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

check-guest-memory: $(BUILD)/guest-address-space-test $(BUILD)/guest-memory-file-test
	$(BUILD)/guest-address-space-test
	$(BUILD)/guest-memory-file-test

$(BUILD)/guest-memory-file-test: LinuxRuntime/GuestMemoryFileTest.cpp GuestMemoryFile.h GuestFilePunch.h GuestAddressSpace.h GuestArena.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

$(BUILD)/guest-address-space-test: LinuxRuntime/AddressSpaceTest.cpp GuestAddressSpace.h GuestArena.h GuestElf.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

# Populate with `make debian-sysroot` (Debian 12 amd64 packages).
DEBIAN_ROOT ?= $(abspath $(BUILD)/debian-sysroot)
DEBIAN_TEST_DIR ?= $(BUILD)/debian-runtime
DEBIAN_LIBS = $(DEBIAN_ROOT)/lib/x86_64-linux-gnu
DEBIAN_CRT = $(DEBIAN_ROOT)/usr/lib/x86_64-linux-gnu

.PHONY: debian-runtime
debian-runtime: $(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf $(BUILD)/process-probe-debian.elf
	python3 LinuxRuntime/StageDebian.py "$(DEBIAN_ROOT)" "$(DEBIAN_TEST_DIR)" $^

.PHONY: debian-sysroot
debian-sysroot:
	python3 -I LinuxRuntime/FetchDebian.py "$(DEBIAN_ROOT)"

FILESYSTEM_TEST_DIR ?= $(BUILD)/guest/sd
.PHONY: rootfs-manifest
rootfs-manifest: $(BUILD)/debian-rootfs.manifest

$(BUILD)/debian-rootfs.manifest: LinuxRuntime/RootfsManifest.py LinuxRuntime/FetchDebian.py
	@mkdir -p $(BUILD)
	python3 -I LinuxRuntime/RootfsManifest.py $@

.PHONY: filesystem-runtime
filesystem-runtime: $(BUILD)/debian-rootfs.manifest $(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf $(BUILD)/process-probe-debian.elf $(BUILD)/filesystem-probe-debian.elf $(BUILD)/fs-suite-debian.elf $(BUILD)/name-limit-debian.elf $(BUILD)/proc-suite-debian.elf $(BUILD)/process-acceptance-debian.elf $(BUILD)/comm-suite-debian.elf $(BUILD)/comm-acceptance-debian.elf $(BUILD)/container-suite-debian.elf $(BUILD)/container-acceptance-debian.elf
	python3 LinuxRuntime/StageDebian.py "$(DEBIAN_ROOT)" "$(FILESYSTEM_TEST_DIR)" $(wordlist 2,4,$^) --filesystem-probe $(word 5,$^) --fs-suite $(word 6,$^) --name-limit-probe $(word 7,$^) --proc-suite $(word 8,$^) --process-acceptance $(word 9,$^) --comm-suite $(word 10,$^) --comm-acceptance $(word 11,$^) --container-suite $(word 12,$^) --container-acceptance $(word 13,$^) --rootfs-manifest $(word 1,$^)

# Filesystem compatibility suite: the Debian guest ELF and a native reference
# run on the host kernel, as the current user and, where user namespaces
# with subordinate IDs are available, as namespace root.
.PHONY: fs-suite check-fs-suite-native
fs-suite: $(BUILD)/fs-suite-debian.elf

$(BUILD)/fs-suite.o: LinuxRuntime/FsSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

# Measures the backing filesystem's name and path length limits.
$(BUILD)/name-limit.o: LinuxRuntime/NameLimitProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/fs-suite-host: LinuxRuntime/FsSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

check-fs-suite-native: $(BUILD)/fs-suite-host
	mkdir -p $(BUILD)/fs-suite-native
	cd $(BUILD)/fs-suite-native && { ../fs-suite-host; test $$? -eq 42; }
	if unshare --map-root-user --map-auto true 2>/dev/null; then \
		cd $(BUILD)/fs-suite-native && { unshare --map-root-user --map-auto ../fs-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-auto is unavailable"; \
	fi

# Process and event compatibility suite: same layout as the filesystem suite.
.PHONY: proc-suite check-proc-suite-native
proc-suite: $(BUILD)/proc-suite-debian.elf

$(BUILD)/proc-suite.o: LinuxRuntime/ProcSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/process-acceptance.o: LinuxRuntime/ProcessAcceptance.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/proc-suite-host: LinuxRuntime/ProcSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

check-proc-suite-native: $(BUILD)/proc-suite-host
	mkdir -p $(BUILD)/proc-suite-native
	cd $(BUILD)/proc-suite-native && { ../proc-suite-host; test $$? -eq 42; }
	if unshare --map-root-user --map-auto true 2>/dev/null; then \
		cd $(BUILD)/proc-suite-native && { unshare --map-root-user --map-auto ../proc-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-auto is unavailable"; \
	fi

# Memory and communication suite (milestone 3): same layout as the process
# suite. The native reference needs no network beyond 127.0.0.1.
.PHONY: comm-suite check-comm-suite-native
comm-suite: $(BUILD)/comm-suite-debian.elf

$(BUILD)/comm-suite.o: LinuxRuntime/CommSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/comm-acceptance.o: LinuxRuntime/CommAcceptance.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/comm-suite-host: LinuxRuntime/CommSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -pthread -Wall -Wextra -Werror $< -o $@

check-comm-suite-native: $(BUILD)/comm-suite-host
	mkdir -p $(BUILD)/comm-suite-native
	cd $(BUILD)/comm-suite-native && { ../comm-suite-host; test $$? -eq 42; }
	if unshare --map-root-user --map-auto true 2>/dev/null; then \
		cd $(BUILD)/comm-suite-native && { unshare --map-root-user --map-auto ../comm-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-auto is unavailable"; \
	fi

# Container and scheduling suite (milestone 4): same layout as the
# communication suite.
.PHONY: container-suite check-container-suite-native
container-suite: $(BUILD)/container-suite-debian.elf

$(BUILD)/container-suite.o: LinuxRuntime/ContainerSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/container-acceptance.o: LinuxRuntime/ContainerAcceptance.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/container-suite-host: LinuxRuntime/ContainerSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -pthread -Wall -Wextra -Werror $< -o $@

check-container-suite-native: $(BUILD)/container-suite-host
	mkdir -p $(BUILD)/container-suite-native
	cd $(BUILD)/container-suite-native && { ../container-suite-host; test $$? -eq 42; }
	if unshare --map-root-user --map-auto true 2>/dev/null; then \
		cd $(BUILD)/container-suite-native && { unshare --map-root-user --map-auto ../container-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-auto is unavailable"; \
	fi

.PHONY: filesystem-probe
filesystem-probe: $(BUILD)/filesystem-probe-debian.elf

$(BUILD)/filesystem-probe.o: LinuxRuntime/FilesystemProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf $(BUILD)/process-probe-debian.elf $(BUILD)/filesystem-probe-debian.elf $(BUILD)/fs-suite-debian.elf $(BUILD)/name-limit-debian.elf $(BUILD)/proc-suite-debian.elf $(BUILD)/process-acceptance-debian.elf $(BUILD)/comm-suite-debian.elf $(BUILD)/comm-acceptance-debian.elf $(BUILD)/container-suite-debian.elf $(BUILD)/container-acceptance-debian.elf: $(BUILD)/%-debian.elf: $(BUILD)/%.o $(DEBIAN_CRT)/Scrt1.o $(DEBIAN_CRT)/crti.o $(DEBIAN_CRT)/crtn.o $(DEBIAN_LIBS)/libc.so.6
	$(HOST_X86_LD) -m elf_x86_64 -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 -rpath-link "$(DEBIAN_LIBS)" \
		"$(DEBIAN_CRT)/Scrt1.o" "$(DEBIAN_CRT)/crti.o" $< "$(DEBIAN_LIBS)/libc.so.6" "$(DEBIAN_CRT)/crtn.o" -o $@

$(BUILD)/thread-ipc.o: LinuxRuntime/ThreadIpcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/glibc-probe.o: LinuxRuntime/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/process-probe.o: LinuxRuntime/ProcessProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

.PHONY: check-guest-log
check-guest-log: $(BUILD)/guest-log-buffer-test
	$<

$(BUILD)/guest-log-buffer-test: LinuxRuntime/GuestLogBufferTest.cpp GuestLogBuffer.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-terminal
check-guest-terminal: $(BUILD)/guest-terminal-test
	$<

$(BUILD)/guest-terminal-test: LinuxRuntime/GuestTerminalTest.cpp GuestTerminal.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-ftp
check-guest-ftp: $(BUILD)/ftp-server-host
	python3 LinuxRuntime/FtpTest.py --server $<

$(BUILD)/ftp-server-host: LinuxRuntime/FtpServerHost.cpp GuestFtp.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-inode-store
check-guest-inode-store: $(BUILD)/guest-inode-store-test
	$<

$(BUILD)/guest-inode-store-test: LinuxRuntime/GuestInodeStoreTest.cpp GuestInodeStore.h GuestFileExtents.h GuestPosixAcl.h GuestDirectoryRecovery.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-file-write
check-guest-file-write: $(BUILD)/guest-file-write-test
	$<

$(BUILD)/guest-file-write-test: LinuxRuntime/GuestFileWriteTest.cpp GuestFileWrite.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-file-punch
check-guest-file-punch: $(BUILD)/guest-file-punch-test
	$<

$(BUILD)/guest-file-punch-test: LinuxRuntime/GuestFilePunchTest.cpp GuestFilePunch.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-preempt
check-guest-preempt: $(BUILD)/guest-preempt-test
	$<

$(BUILD)/guest-preempt-test: LinuxRuntime/GuestPreemptTest.cpp GuestPreempt.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-credentials
check-guest-credentials: $(BUILD)/guest-credentials-test
	$<

$(BUILD)/guest-credentials-test: LinuxRuntime/GuestCredentialsTest.cpp GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-clocks
check-guest-clocks: $(BUILD)/guest-clocks-test
	$<

$(BUILD)/guest-clocks-test: LinuxRuntime/GuestClocksTest.cpp GuestClocks.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-eventfd
check-guest-eventfd: $(BUILD)/guest-eventfd-test
	$<

$(BUILD)/guest-eventfd-test: LinuxRuntime/GuestEventFdTest.cpp GuestEventFd.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-inet
check-guest-inet: $(BUILD)/guest-inet-test
	$<

$(BUILD)/guest-inet-test: LinuxRuntime/GuestInetTest.cpp GuestInet.h GuestSocket.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-sched
check-guest-sched: $(BUILD)/guest-sched-test
	$<

$(BUILD)/guest-sched-test: LinuxRuntime/GuestSchedTest.cpp GuestSched.h GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-namespaces
check-guest-namespaces: $(BUILD)/guest-namespaces-test
	$<

$(BUILD)/guest-namespaces-test: LinuxRuntime/GuestNamespacesTest.cpp GuestNamespaces.h GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-mounts
check-guest-mounts: $(BUILD)/guest-mounts-test
	$<

$(BUILD)/guest-mounts-test: LinuxRuntime/GuestMountsTest.cpp GuestMounts.h GuestNamespaces.h GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-seccomp
check-guest-seccomp: $(BUILD)/guest-seccomp-test
	$<

$(BUILD)/guest-seccomp-test: LinuxRuntime/GuestSeccompTest.cpp GuestSeccomp.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-futex
check-guest-futex: $(BUILD)/guest-futex-test
	$<

$(BUILD)/guest-futex-test: LinuxRuntime/GuestFutexTest.cpp GuestFutex.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-pipe
check-guest-pipe: $(BUILD)/guest-pipe-packet-test
	$<

$(BUILD)/guest-pipe-packet-test: LinuxRuntime/GuestPipePacketTest.cpp GuestSocket.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-socket
check-guest-socket: $(BUILD)/guest-socket-test
	$<

$(BUILD)/guest-socket-test: LinuxRuntime/GuestSocketTest.cpp GuestSocket.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-sysv-ipc
check-guest-sysv-ipc: $(BUILD)/guest-sysv-ipc-test
	$<

$(BUILD)/guest-sysv-ipc-test: LinuxRuntime/GuestSysvIpcTest.cpp GuestSysvIpc.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-mqueue
check-guest-mqueue: $(BUILD)/guest-mqueue-test
	$<

$(BUILD)/guest-mqueue-test: LinuxRuntime/GuestMqueueTest.cpp GuestMqueue.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-signals
check-guest-signals: $(BUILD)/guest-signals-test
	$<

$(BUILD)/guest-signals-test: LinuxRuntime/GuestSignalsTest.cpp GuestSignals.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-job-control
check-guest-job-control: $(BUILD)/guest-job-control-test
	$<

$(BUILD)/guest-job-control-test: LinuxRuntime/GuestJobControlTest.cpp GuestJobControl.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-inotify
check-guest-inotify: $(BUILD)/guest-inotify-test
	$<

$(BUILD)/guest-inotify-test: LinuxRuntime/GuestInotifyTest.cpp GuestInotify.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-epoll
check-guest-epoll: $(BUILD)/guest-epoll-test
	$<

$(BUILD)/guest-epoll-test: LinuxRuntime/GuestEpollTest.cpp GuestEpoll.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-timers
check-guest-timers: $(BUILD)/guest-timers-test
	$<

$(BUILD)/guest-timers-test: LinuxRuntime/GuestTimersTest.cpp GuestTimers.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

.PHONY: check-guest-script
check-guest-script: $(BUILD)/guest-script-test
	$<

$(BUILD)/guest-script-test: LinuxRuntime/GuestScriptTest.cpp GuestScript.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@

# Debian rootfs setup (launcher menu): unpacker checks against Python's
# ar/tar readers on every package of the manifest.
XZ_EMBEDDED := ThirdParty/xz-embedded
XZ_DEFINES := -DXZ_USE_CRC64 -DXZ_USE_SHA256 -DXZ_DEC_X86 -DXZ_DEC_CONCATENATED
XZ_SOURCES := $(addprefix $(XZ_EMBEDDED)/,xz_crc32.c xz_crc64.c xz_sha256.c xz_dec_stream.c xz_dec_lzma2.c xz_dec_bcj.c)

$(BUILD)/guest-rootfs-setup-test: LinuxRuntime/GuestRootfsSetupTest.cpp GuestRootfsSetup.h $(XZ_SOURCES) | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -I. $(XZ_DEFINES) -c LinuxRuntime/GuestRootfsSetupTest.cpp -o $@.o
	$(foreach Source,$(XZ_SOURCES),cc -std=gnu11 -O2 -I$(XZ_EMBEDDED) $(XZ_DEFINES) -c $(Source) -o $(BUILD)/$(notdir $(Source:.c=.o)) &&) true
	$(HOST_CXX) $@.o $(addprefix $(BUILD)/,$(notdir $(XZ_SOURCES:.c=.o))) -o $@

.PHONY: check-rootfs-setup
check-rootfs-setup: $(BUILD)/guest-rootfs-setup-test $(BUILD)/debian-rootfs.manifest
	$(BUILD)/guest-rootfs-setup-test
	python3 -I LinuxRuntime/CheckRootfsSetup.py $(BUILD)/debian-rootfs.manifest $(BUILD)/rootfs-manifest-cache $(BUILD)/guest-rootfs-setup-test $(BUILD)/rootfs-setup-native
