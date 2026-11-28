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

# Include strategy: no relative includes. Every source folder with headers is on one
# include path (INC) and prerequisites resolve bare header names through vpath.
SRC_INC_DIRS := runtime runtime/fs runtime/process runtime/memory runtime/ipc host
INC := $(addprefix -I,$(SRC_INC_DIRS)) -I.
vpath %.h $(SRC_INC_DIRS)
vpath %.cpp probes

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

# EXPERIMENT download probe: the RootFS image over plain HTTP (Range resume, throughput, SD write) with the installer's HTTP client.
# `make download-probe` -> build/fex-switch2-downloadprobe.nro, log sdmc:/switch/fex-downloadprobe.log.
DLPROBE := $(BUILD)/fex-switch2-downloadprobe
.PHONY: download-probe
download-probe: $(DLPROBE).nro
$(BUILD)/DownloadProbe.o: CPPFLAGS += $(INC) -I$(ROOT)/External/xxhash
$(DLPROBE).elf: $(BUILD)/DownloadProbe.o Makefile
	$(LD) $(LDFLAGS) $(BUILD)/DownloadProbe.o $(OVERLAY_LIBPATHS) $(LIBPATHS) -lnx -o $@
$(DLPROBE).nacp: APP_TITLE := FEX Switch 2 Download Probe
$(DLPROBE).nacp: | $(BUILD)
$(DLPROBE).nro: $(DLPROBE).elf $(DLPROBE).nacp $(APP_ICON)
$(DLPROBE).nro: NROFLAGS = --icon=$(APP_ICON) --nacp=$(DLPROBE).nacp

# EXPERIMENT multithreading probe (docs/MultithreadingPlan.md Task 0): links FEXCore, so it builds in the launcher tree.
# `make mt-probe` -> build/guest/fex-switch2-mtprobe.nro, log sdmc:/fex/mtprobe.log.
.PHONY: mt-probe
mt-probe:
	cmake --build build/guest --target mtprobe_nro

# EXPERIMENT UI probe: Vulkan on OunceVK behind the ui/platform and ui/render interfaces (ounce-hbmenu's approach).
# `make ui-probe` -> build/fex-switch2-uiprobe.nro, log sdmc:/switch/fex-uiprobe.log.
# `make ui-probe-host` -> build/fex-uiprobe-host (same probe, headless Vulkan on the Linux GPU, writes a PNG).
# `make check-ui` builds the host probe, renders it at two scales and checks pixels, and checks the include rules.
# Shaders: ui/render/shaders/ui.{vert,frag} are compiled to SPIR-V headers with glslangValidator (as hbmenu does).
UI_DIR := ui
UIPROBE := $(BUILD)/fex-switch2-uiprobe
UI_GEN := $(BUILD)/ui-gen
UI_SW2 := $(BUILD)/ui-switch2
UI_HOST := $(BUILD)/ui-host
# Fonts and icons are linked into the binary (ui/assets/Assets.cpp .incbin), so the NRO needs nothing on the SD card.
UI_ASSET_DIR := $(abspath $(UI_DIR)/assets)
UI_ASSET_FILES := $(wildcard $(UI_DIR)/assets/*.json $(UI_DIR)/assets/fonts/*.ttf $(UI_DIR)/assets/icons/*.svg $(UI_DIR)/assets/licenses/*.txt $(UI_DIR)/assets/licenses.json)
UI_DEFS := -DUI_ASSET_DIR='"$(UI_ASSET_DIR)"'
UI_COMMON := platform/common/Log platform/common/Names render/vulkan/Target render/vulkan/VulkanRenderer \
  render/Atlas render/Fonts render/Icons assets/Assets assets/IndexAsset theme theme_dark theme_light \
  input/InputModes input/Focus input/Interaction components/Basic components/Chrome components/Controls components/Modals components/Progress screens/gallery \
  components/Rows components/LicenseModal Storage PathUtil Json RootfsIndex SettingsSchema LicenseStore FileBrowser screens/home screens/settings screens/filepicker \
  components/Tiles components/Terminal TerminalGrid TerminalKeys FtpModel InstallModel RootfsSelection screens/ftp screens/rootfs_setup screens/install screens/shell \
  App RootfsState
UI_SW2_SRCS := $(UI_COMMON) platform/switch2/Switch2Platform platform/switch2/Switch2AppInit
UI_HOST_SRCS := $(UI_COMMON) platform/host/HostPlatform platform/host/PngWriter backend/FakeBackend
UI_SW2_OBJS := $(addprefix $(UI_SW2)/,$(addsuffix .o,$(UI_SW2_SRCS))) $(UI_SW2)/UiProbe.o $(UI_SW2)/UiProbeSwitch2Main.o
UI_HOST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/UiProbe.o $(UI_HOST)/UiProbeHostMain.o
UI_SHADERS := $(UI_GEN)/ui_vert.h $(UI_GEN)/ui_frag.h
UI_INC := -I$(UI_DIR) -I. -I$(UI_GEN)

$(UI_GEN)/ui_%.h: $(UI_DIR)/render/shaders/ui.%
	@mkdir -p $(@D)
	glslangValidator -V --quiet --vn ui_$* -o $@ $<

# Console objects.
UI_SW2_INC := $(UI_INC) -Iruntime/process -I$(PORTLIBS)/include/freetype2 -I$(PORTLIBS)/include/harfbuzz $(UI_DEFS)
$(UI_SW2)/%.o: $(UI_DIR)/%.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(UI_SW2_INC) -MMD -MP -c $< -o $@
$(UI_SW2)/%.o: probes/%.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(UI_SW2_INC) -MMD -MP -c $< -o $@
$(UI_SW2)/assets/Assets.o $(UI_SW2)/assets/IndexAsset.o: $(UI_ASSET_FILES)
$(UIPROBE).elf: $(UI_SW2_OBJS) Makefile
	$(LD) $(LDFLAGS) $(UI_SW2_OBJS) $(OVERLAY_LIBPATHS) -L$(PORTLIBS)/lib $(LIBPATHS) \
		-lvulkan -lharfbuzz -lfreetype -lpng16 -lbz2 -lz -lm -lnx -o $@
$(UIPROBE).nacp: APP_TITLE := FEX Switch 2 UI Probe
$(UIPROBE).nacp: | $(BUILD)
$(UIPROBE).nro: $(UIPROBE).elf $(UIPROBE).nacp $(APP_ICON)
$(UIPROBE).nro: NROFLAGS = --icon=$(APP_ICON) --nacp=$(UIPROBE).nacp
.PHONY: ui-probe ui-probe-host
ui-probe: $(UIPROBE).nro

# Host objects (system Vulkan loader, FreeType and zlib).
HOST_CXX ?= c++
UI_HOST_FLAGS := -std=gnu++20 -O2 -g -Wall -Wextra -fno-exceptions $(UI_INC) $(UI_DEFS) $(shell pkg-config --cflags freetype2 harfbuzz)
UI_HOST_LIBS = $$(pkg-config --libs freetype2 harfbuzz vulkan zlib)
$(UI_HOST)/%.o: $(UI_DIR)/%.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(UI_HOST)/%.o: probes/%.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(UI_HOST)/assets/Assets.o $(UI_HOST)/assets/IndexAsset.o: $(UI_ASSET_FILES)
$(BUILD)/fex-uiprobe-host: $(UI_HOST_OBJS) Makefile
	$(HOST_CXX) $(UI_HOST_OBJS) $(UI_HOST_LIBS) -o $@
ui-probe-host: $(BUILD)/fex-uiprobe-host

# `make ui-host` -> build/fex-ui-host: renders a screen headless to PNG (--screen swatch --theme dark|light --size WxH --out f.png).
UI_APP_SRCS := HostMain screens/swatch
UI_APP_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_APP_SRCS))) $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS)))
$(BUILD)/fex-ui-host: $(UI_APP_OBJS) Makefile
	$(HOST_CXX) $(UI_APP_OBJS) $(UI_HOST_LIBS) -o $@
.PHONY: ui-host
ui-host: $(BUILD)/fex-ui-host

-include $(UI_SW2_OBJS:.o=.d) $(UI_HOST_OBJS:.o=.d) $(UI_APP_OBJS:.o=.d)

# ---- check-ui: host tests, probe screenshots, include rules ----------------------------------------------------------------
UI_TEST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/tests/UiTest.o
UI_TEST_DIR := $(BUILD)/ui-check
UI_SHOT_DIR := $(BUILD)/ui-shots
$(UI_HOST)/tests/UiTest.o: $(UI_DIR)/tests/UiTest.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ui-test: $(UI_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_TEST_OBJS) $(UI_HOST_LIBS) -o $@
-include $(UI_HOST)/tests/UiTest.d

# Task 2 tests: theme, fonts, icons, pixel snapping (tests/RenderTextTest.cpp).
UI_TEXT_TEST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/tests/RenderTextTest.o
$(UI_HOST)/tests/RenderTextTest.o: $(UI_DIR)/tests/RenderTextTest.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ui-text-test: $(UI_TEXT_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_TEXT_TEST_OBJS) $(UI_HOST_LIBS) -o $@
-include $(UI_HOST)/tests/InputTest.d
UI_INPUT_TEST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/tests/InputTest.o
$(UI_HOST)/tests/InputTest.o: $(UI_DIR)/tests/InputTest.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ui-input-test: $(UI_INPUT_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_INPUT_TEST_OBJS) $(UI_HOST_LIBS) -o $@
-include $(UI_HOST)/tests/RenderTextTest.d
-include $(UI_HOST)/tests/Task6Test.d
UI_TASK6_TEST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/tests/Task6Test.o
$(UI_HOST)/tests/Task6Test.o: $(UI_DIR)/tests/Task6Test.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ui-task6-test: $(UI_TASK6_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_TASK6_TEST_OBJS) $(UI_HOST_LIBS) -o $@
-include $(UI_HOST)/tests/AppTest.d
UI_APP_TEST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/tests/AppTest.o
$(UI_HOST)/tests/AppTest.o: $(UI_DIR)/tests/AppTest.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ui-app-test: $(UI_APP_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_APP_TEST_OBJS) $(UI_HOST_LIBS) -o $@
-include $(UI_HOST)/tests/ScreensTest.d
UI_SCREENS_TEST_OBJS := $(addprefix $(UI_HOST)/,$(addsuffix .o,$(UI_HOST_SRCS))) $(UI_HOST)/tests/ScreensTest.o
$(UI_HOST)/tests/ScreensTest.o: $(UI_DIR)/tests/ScreensTest.cpp Makefile $(UI_SHADERS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(UI_HOST_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ui-screens-test: $(UI_SCREENS_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_SCREENS_TEST_OBJS) $(UI_HOST_LIBS) -o $@

UI_TERMINAL_TEST_OBJS := $(addprefix $(UI_HOST)/,TerminalGrid.o TerminalKeys.o tests/TerminalTest.o)
-include $(UI_HOST)/tests/TerminalTest.d
$(BUILD)/ui-terminal-test: $(UI_TERMINAL_TEST_OBJS) Makefile
	$(HOST_CXX) $(UI_TERMINAL_TEST_OBJS) -o $@

# A hex color literal (0xRRGGBB or "#RRGGBB") outside the two theme files fails the build.
UI_HEX_LITERAL := '(0[xX][0-9A-Fa-f]{6}([^0-9A-Fa-f]|$$))|(\#[0-9A-Fa-f]{6}([^0-9A-Fa-f]|$$))'
.PHONY: check-ui-colors
check-ui-colors:
	@bad=$$( grep -rEn $(UI_HEX_LITERAL) $(UI_DIR) probes/UiProbe.cpp probes/UiProbe.h probes/UiProbeHostMain.cpp probes/UiProbeSwitch2Main.cpp \
	           --include='*.h' --include='*.cpp' --include='*.hpp' --include='*.vert' --include='*.frag' \
	           --exclude=theme_dark.cpp --exclude=theme_light.cpp --exclude-dir=assets ); \
	if [ -n "$$bad" ]; then echo "check-ui: hex color literal outside ui/theme_dark.cpp and ui/theme_light.cpp:"; echo "$$bad"; exit 1; fi
	@echo "check-ui: no hex color literals outside the theme files"

# Vulkan and libnx headers only in ui/platform/ and ui/render/. The interface headers directly in those two folders
# (Display.h, Input.h, System.h, Platform.h, Log.h, Renderer.h) must not include them either. probes/UiProbeSwitch2Main.cpp
# is the console program entry and uses libnx for the text-console failure fallback, so it is exempt.
UI_BANNED_INCLUDE := '^[[:space:]]*\#[[:space:]]*include[[:space:]]*[<"](vulkan[/.]|vk_|switch\.h|switch/)'
.PHONY: check-ui check-ui-includes
check-ui-includes:
	@bad=$$( { grep -rEn $(UI_BANNED_INCLUDE) $(UI_DIR) --include='*.h' --include='*.cpp' --include='*.hpp' \
	             --exclude-dir=assets --exclude-dir=platform --exclude-dir=render; \
	           grep -En $(UI_BANNED_INCLUDE) $(UI_DIR)/platform/*.h $(UI_DIR)/render/*.h; \
	           grep -En $(UI_BANNED_INCLUDE) probes/UiProbe.cpp probes/UiProbe.h probes/UiProbeHostMain.cpp; } ); \
	if [ -n "$$bad" ]; then echo "check-ui: Vulkan or libnx include outside ui/platform and ui/render:"; echo "$$bad"; exit 1; fi
	@echo "check-ui: include rules ok"

check-ui: check-ui-includes check-ui-colors $(BUILD)/ui-terminal-test $(BUILD)/ui-test $(BUILD)/ui-text-test $(BUILD)/ui-input-test $(BUILD)/ui-screens-test $(BUILD)/ui-task6-test $(BUILD)/ui-app-test $(BUILD)/fex-uiprobe-host $(BUILD)/fex-ui-host
	$(BUILD)/ui-terminal-test
	$(BUILD)/ui-test
	$(BUILD)/ui-text-test
	$(BUILD)/ui-input-test
	$(BUILD)/ui-screens-test
	$(BUILD)/ui-task6-test
	$(BUILD)/ui-app-test
	@mkdir -p $(UI_TEST_DIR)
	$(BUILD)/ui-text-test --dump-theme > $(UI_TEST_DIR)/theme-dump.txt
	python3 -I $(UI_DIR)/tests/CheckTheme.py $(UI_TEST_DIR)/theme-dump.txt $(abspath docs/ui/fex-switch2-theming-spec.md)
	$(BUILD)/fex-ui-host --headless --screen swatch --theme dark --size 1920x1080 --out $(UI_TEST_DIR)/swatch-dark-1080.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen swatch --theme light --size 1920x1080 --out $(UI_TEST_DIR)/swatch-light-1080.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen swatch --theme dark --size 2560x1440 --out $(UI_TEST_DIR)/swatch-dark-1440.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen swatch --theme light --size 2560x1440 --out $(UI_TEST_DIR)/swatch-light-1440.png >/dev/null
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/swatch-dark-1080.png 1920x1080 eq:0,0,27,27,31 eq:1919,1079,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/swatch-light-1080.png 1920x1080 eq:0,0,235,235,239 eq:1919,1079,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/swatch-dark-1440.png 2560x1440 eq:0,0,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/swatch-light-1440.png 2560x1440 eq:0,0,235,235,239
	$(BUILD)/fex-ui-host --headless --screen gallery --theme dark --size 1920x1080 --out $(UI_TEST_DIR)/gallery-dark-1080.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme light --size 1920x1080 --out $(UI_TEST_DIR)/gallery-light-1080.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme dark --size 1280x720 --out $(UI_TEST_DIR)/gallery-dark-720.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme dark --size 1920x1080 --scroll 520 --out $(UI_TEST_DIR)/gallery-dark-1080-controls.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme dark --size 1920x1080 --scroll 1200 --out $(UI_TEST_DIR)/gallery-dark-1080-log.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme dark --size 1920x1080 --modal picker --out $(UI_TEST_DIR)/gallery-dark-1080-picker.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme light --size 1920x1080 --modal destructive --out $(UI_TEST_DIR)/gallery-light-1080-confirm.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen gallery --theme dark --size 2560x1440 --script D,R --out $(UI_TEST_DIR)/gallery-dark-1440.png >/dev/null
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:90,450,42,42,48 eq:74,228,0,195,227 any:60,20,500,100,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-dark-720.png 1280x720 eq:2,2,27,27,31 eq:60,300,42,42,48 eq:50,152,0,195,227
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-light-1080.png 1920x1080 eq:2,2,235,235,239 any:60,20,500,100,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-dark-1080-controls.png 1920x1080 eq:2,2,27,27,31 any:60,150,1860,1000,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-dark-1080-log.png 1920x1080 eq:2,2,27,27,31 eq:100,700,18,18,21 any:60,150,1860,1000,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-dark-1080-picker.png 1920x1080 ne:30,450,27,27,31 ne:30,450,42,42,48 eq:960,540,52,52,59 eq:585,540,42,42,48
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-light-1080-confirm.png 1920x1080 ne:30,450,235,235,239 any:600,350,1320,700,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/gallery-dark-1440.png 2560x1440 eq:2,2,27,27,31
	@mkdir -p $(UI_TEST_DIR)
	@# Task 4 screens (FakeBackend): every screen in both themes, the modals, large folder and loading states.
	for th in dark light; do \
	  for sc in home settings about filepicker; do \
	    $(BUILD)/fex-ui-host --headless --screen $$sc --theme $$th --size 1920x1080 --out $(UI_TEST_DIR)/$$sc-$$th-1080.png >/dev/null || exit 1; \
	  done; \
	done
	$(BUILD)/fex-ui-host --headless --screen home --theme dark --size 1280x720 --out $(UI_TEST_DIR)/home-dark-720.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen settings --theme light --size 1280x720 --out $(UI_TEST_DIR)/settings-light-720.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen filepicker --theme dark --size 3840x2160 --out $(UI_TEST_DIR)/filepicker-dark-2160.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen settings --theme dark --size 1920x1080 --section 1 --out $(UI_TEST_DIR)/settings-dark-1080-appearance.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen settings --theme dark --size 1920x1080 --modal wipe --out $(UI_TEST_DIR)/settings-dark-1080-wipe.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen settings --theme light --size 1920x1080 --modal restore --out $(UI_TEST_DIR)/settings-light-1080-restore.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen settings --theme dark --size 1920x1080 --modal enum --out $(UI_TEST_DIR)/settings-dark-1080-picker.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen about --theme dark --size 1920x1080 --modal license --out $(UI_TEST_DIR)/about-dark-1080-license.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen about --theme light --size 1920x1080 --modal text --out $(UI_TEST_DIR)/about-light-1080-text.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen filepicker --theme dark --size 1920x1080 --picker folder --out $(UI_TEST_DIR)/filepicker-dark-1080-folder.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen filepicker --theme dark --size 1920x1080 --picker any --many 5000 --path sdmc:/roms --script ........,R,Z,Z --frames 60 --out $(UI_TEST_DIR)/filepicker-dark-1080-many.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen filepicker --theme light --size 1920x1080 --slow 100 --frames 20 --out $(UI_TEST_DIR)/filepicker-light-1080-loading.png >/dev/null
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/home-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:1264,362,42,42,48 ne:300,400,27,27,31 any:480,160,1440,260,27,27,31 eq:722,889,44,44,50 any:1240,1000,1860,1060,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/home-light-1080.png 1920x1080 eq:2,2,235,235,239 any:480,160,1440,260,235,235,239 any:1240,1000,1860,1060,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/home-dark-720.png 1280x720 eq:2,2,27,27,31 eq:843,240,42,42,48 any:320,100,960,150,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:60,180,42,42,48 any:140,40,500,100,27,27,31 any:500,230,1850,300,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-light-1080.png 1920x1080 eq:2,2,235,235,239 any:140,40,500,100,235,235,239 any:500,230,1850,300,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-light-720.png 1280x720 eq:2,2,235,235,239 any:90,20,300,70,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-dark-1080-appearance.png 1920x1080 eq:2,2,27,27,31 eq:60,270,42,42,48 any:500,170,1850,230,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-dark-1080-wipe.png 1920x1080 ne:30,450,27,27,31 eq:960,540,42,42,48 any:560,380,1360,700,42,42,48
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-light-1080-restore.png 1920x1080 ne:30,450,235,235,239 any:560,380,1360,700,255,255,255
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/settings-dark-1080-picker.png 1920x1080 ne:30,450,27,27,31 eq:960,540,42,42,48
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/about-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:510,200,44,44,50 any:640,160,1500,300,27,27,31 any:500,370,1850,420,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/about-light-1080.png 1920x1080 eq:2,2,235,235,239 any:640,160,1500,300,235,235,239 any:500,370,1850,420,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/about-dark-1080-license.png 1920x1080 ne:30,450,27,27,31 eq:1300,335,42,42,48 any:520,350,1400,620,42,42,48
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/about-light-1080-text.png 1920x1080 eq:2,500,235,235,239 any:70,150,900,950,235,235,239 any:1300,1000,1860,1060,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/filepicker-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:60,280,42,42,48 any:140,30,600,70,27,27,31 any:520,250,1850,330,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/filepicker-light-1080.png 1920x1080 eq:2,2,235,235,239 any:140,30,600,70,235,235,239 any:520,250,1850,330,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/filepicker-dark-2160.png 3840x2160 eq:2,2,27,27,31 any:280,60,1200,140,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/filepicker-dark-1080-folder.png 1920x1080 eq:2,2,27,27,31 any:140,70,600,110,27,27,31 any:140,1000,900,1060,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/filepicker-dark-1080-many.png 1920x1080 eq:2,2,27,27,31 any:520,260,1850,950,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/filepicker-light-1080-loading.png 1920x1080 eq:2,2,235,235,239 any:1130,540,1220,620,235,235,239
	@# Task 6 screens (FakeBackend): FTP, RootFS setup, Installation, Shell in both themes, plus their modals and states, into build/ui-shots.
	@mkdir -p $(UI_SHOT_DIR)
	for th in dark light; do \
	  $(BUILD)/fex-ui-host --headless --screen ftp --theme $$th --size 1920x1080 --out $(UI_SHOT_DIR)/ftp-$$th-1080.png >/dev/null || exit 1; \
	  $(BUILD)/fex-ui-host --headless --screen rootfs --theme $$th --size 1920x1080 --out $(UI_SHOT_DIR)/rootfs-$$th-1080.png >/dev/null || exit 1; \
	  $(BUILD)/fex-ui-host --headless --screen install --install running --theme $$th --size 1920x1080 --out $(UI_SHOT_DIR)/install-$$th-1080.png >/dev/null || exit 1; \
	  $(BUILD)/fex-ui-host --headless --screen shell --theme $$th --size 1920x1080 --out $(UI_SHOT_DIR)/shell-$$th-1080.png >/dev/null || exit 1; \
	done
	$(BUILD)/fex-ui-host --headless --screen ftp --ftp stopped --theme dark --size 1920x1080 --out $(UI_SHOT_DIR)/ftp-dark-1080-stopped.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen ftp --ftp nonet --theme light --size 1920x1080 --script Y --out $(UI_SHOT_DIR)/ftp-light-1080-nonet.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen ftp --theme dark --size 1920x1080 --modal disconnect --out $(UI_SHOT_DIR)/ftp-dark-1080-disconnect.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen ftp --theme dark --size 1280x720 --out $(UI_SHOT_DIR)/ftp-dark-720.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen rootfs --reinstall --theme dark --size 1920x1080 --out $(UI_SHOT_DIR)/rootfs-dark-1080-reinstall.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen rootfs --reinstall --theme light --size 1920x1080 --modal reinstall --out $(UI_SHOT_DIR)/rootfs-light-1080-reinstall-confirm.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen rootfs --space low --theme dark --size 1920x1080 --out $(UI_SHOT_DIR)/rootfs-dark-1080-lowspace.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen rootfs --theme dark --size 1920x1080 --scroll 400 --out $(UI_SHOT_DIR)/rootfs-dark-1080-options.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen install --install cancel --theme dark --size 1920x1080 --out $(UI_SHOT_DIR)/install-dark-1080-cancel.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen install --install done --theme light --size 1920x1080 --out $(UI_SHOT_DIR)/install-light-1080-done.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen install --install failed --theme dark --size 1920x1080 --out $(UI_SHOT_DIR)/install-dark-1080-failed.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen shell --theme dark --size 1280x720 --out $(UI_SHOT_DIR)/shell-dark-720.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen shell --shell tabs --theme light --size 1920x1080 --out $(UI_SHOT_DIR)/shell-light-1080-tabs.png >/dev/null
	$(BUILD)/fex-ui-host --headless --screen shell --theme dark --size 1920x1080 --modal exit --out $(UI_SHOT_DIR)/shell-dark-1080-exit.png >/dev/null
	for th in dark light; do for sz in 1280x720 1920x1080 3840x2160; do \
	  $(BUILD)/fex-ui-host --headless --screen shell --theme $$th --size $$sz --script X --out $(UI_SHOT_DIR)/shell-$$th-$$sz-ctrl.png >/dev/null || exit 1; \
	done; done
	@# X armed: the whole screen darkened (corner), the panel in the middle (Surface), at the three sizes.
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-dark-1920x1080-ctrl.png 1920x1080 eq:2,2,16,18,19 eq:930,385,242,242,244 eq:960,540,13,14,15
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-light-1920x1080-ctrl.png 1920x1080 eq:2,2,145,146,147 eq:930,385,27,27,31 eq:960,540,164,164,164
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-dark-1280x720-ctrl.png 1280x720 eq:2,2,16,18,19 eq:620,257,242,242,244 eq:640,360,13,14,15
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-dark-3840x2160-ctrl.png 3840x2160 eq:2,2,16,18,19 eq:1860,770,242,242,244 eq:1920,1080,13,14,15
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/ftp-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:380,445,42,42,48 any:70,330,1850,460,27,27,31 any:70,560,1850,880,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/ftp-light-1080.png 1920x1080 eq:2,2,235,235,239 any:70,330,1850,460,235,235,239 any:70,560,1850,880,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/ftp-dark-1080-stopped.png 1920x1080 eq:2,2,27,27,31 any:70,200,900,300,27,27,31 any:70,560,900,700,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/ftp-light-1080-nonet.png 1920x1080 eq:2,2,235,235,239 any:70,990,1200,1070,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/ftp-dark-1080-disconnect.png 1920x1080 ne:30,450,27,27,31 eq:960,540,42,42,48
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/ftp-dark-720.png 1280x720 eq:2,2,27,27,31 any:50,220,1230,310,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/rootfs-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:700,330,42,42,48 any:70,200,1850,350,27,27,31 any:70,990,1200,1070,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/rootfs-light-1080.png 1920x1080 eq:2,2,235,235,239 any:70,200,1850,350,235,235,239 any:70,990,1200,1070,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/rootfs-dark-1080-reinstall.png 1920x1080 eq:2,2,27,27,31 any:70,140,1850,220,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/rootfs-light-1080-reinstall-confirm.png 1920x1080 ne:30,450,235,235,239 any:560,380,1360,700,255,255,255
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/rootfs-dark-1080-lowspace.png 1920x1080 eq:2,2,27,27,31 any:70,990,1200,1070,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/rootfs-dark-1080-options.png 1920x1080 eq:2,2,27,27,31 any:70,140,1850,980,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/install-dark-1080.png 1920x1080 eq:2,2,27,27,31 eq:300,320,18,18,21 any:250,440,1680,790,18,18,21 any:240,880,1680,920,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/install-light-1080.png 1920x1080 eq:2,2,235,235,239 any:250,440,1680,790,235,235,239 any:240,880,1680,920,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/install-dark-1080-cancel.png 1920x1080 ne:30,450,27,27,31 eq:960,540,42,42,48
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/install-light-1080-done.png 1920x1080 eq:2,2,235,235,239 any:240,140,1000,220,235,235,239
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/install-dark-1080-failed.png 1920x1080 eq:2,2,27,27,31 any:240,140,1000,220,27,27,31 any:240,990,1000,1070,27,27,31
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-dark-1080.png 1920x1080 eq:1000,20,42,46,50 eq:900,700,35,38,39 any:30,90,1200,420,35,38,39
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-light-1080.png 1920x1080 eq:1000,20,222,224,226 eq:900,700,252,252,252 any:30,90,1200,420,252,252,252
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-dark-720.png 1280x720 eq:700,15,42,46,50 eq:600,450,35,38,39 any:20,60,800,290,35,38,39
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-light-1080-tabs.png 1920x1080 eq:1500,20,222,224,226 any:0,0,700,60,222,224,226
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/shell-dark-1080-exit.png 1920x1080 ne:30,450,35,38,39 eq:960,540,42,42,48
	@# Task 7: every screen in both themes at 1280x720, 1920x1080 and 3840x2160 (FakeBackend), into build/ui-shots.
	@mkdir -p $(UI_SHOT_DIR)
	for sc in home settings about filepicker ftp rootfs install shell; do \
	  for th in dark light; do \
	    for sz in 1280x720 1920x1080 3840x2160; do \
	      h=$${sz#*x}; \
	      $(BUILD)/fex-ui-host --headless --screen $$sc --theme $$th --size $$sz --out $(UI_SHOT_DIR)/$$sc-$$th-$${h}p.png >/dev/null || exit 1; \
	      python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_SHOT_DIR)/$$sc-$$th-$${h}p.png $$sz || exit 1; \
	    done; \
	  done; \
	done
	$(BUILD)/fex-uiprobe-host --headless --size 1280x720 --frames 3 --out $(UI_TEST_DIR)/probe-720.png --log $(UI_TEST_DIR)/probe-720.log >/dev/null
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/probe-720.png 1280x720 \
	  eq:0,0,27,27,31 eq:1279,719,27,27,31 \
	  eq:39,100,27,27,31 eq:40,100,142,184,255 eq:199,100,142,184,255 eq:200,100,27,27,31 \
	  eq:100,89,27,27,31 eq:100,90,142,184,255 eq:100,149,142,184,255 eq:100,150,27,27,31 \
	  eq:600,169,27,27,31 eq:600,170,110,110,124 eq:600,171,27,27,31 \
	  eq:220,90,27,27,31 eq:300,120,60,64,80 eq:560,120,255,179,71 eq:590,120,27,27,31 \
	  eq:460,120,27,27,31 eq:488,120,142,184,255 \
	  any:40,40,330,70,27,27,31 any:40,190,90,240,27,27,31
	$(BUILD)/fex-uiprobe-host --headless --size 1920x1080 --frames 3 --out $(UI_TEST_DIR)/probe-1080.png --log $(UI_TEST_DIR)/probe-1080.log >/dev/null
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/probe-1080.png 1920x1080 \
	  eq:0,0,27,27,31 eq:1919,1079,27,27,31 \
	  eq:59,150,27,27,31 eq:60,150,142,184,255 eq:299,150,142,184,255 eq:300,150,27,27,31 \
	  eq:100,134,27,27,31 eq:100,135,142,184,255 eq:100,224,142,184,255 eq:100,225,27,27,31 \
	  eq:600,253,27,27,31 eq:600,254,110,110,124 eq:600,255,110,110,124 eq:600,256,27,27,31 \
	  any:60,60,480,105,27,27,31 any:60,285,130,355,27,27,31
	$(BUILD)/fex-uiprobe-host --headless --size 1280x720 --frames 5 --resize-at 2 --resize-to 3840x2160 \
	  --out $(UI_TEST_DIR)/probe-resize.png --log $(UI_TEST_DIR)/probe-resize.log >/dev/null
	python3 -I $(UI_DIR)/tests/CheckPng.py $(UI_TEST_DIR)/probe-resize.png 3840x2160 \
	  eq:119,300,27,27,31 eq:120,300,142,184,255 eq:599,300,142,184,255 eq:600,300,27,27,31
	grep -q 'size change: 1280x720 -> 3840x2160' $(UI_TEST_DIR)/probe-resize.log
	$(BUILD)/fex-uiprobe-host --headless --size 1280x720 --frames 0 --press-plus-at 4 --log $(UI_TEST_DIR)/probe-exit.log >/dev/null
	grep -q 'exit requested (Plus) after 5 frames' $(UI_TEST_DIR)/probe-exit.log
	@echo "check-ui: ok"

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
	$(RM) -r $(UI_GEN) $(UI_SW2) $(UI_HOST) $(UI_TEST_DIR)
	$(RM) $(BUILD)/ui-terminal-test $(BUILD)/ui-test $(BUILD)/ui-text-test $(BUILD)/ui-input-test $(BUILD)/ui-screens-test $(BUILD)/fex-ui-host
	$(RM) $(UIPROBE).elf $(UIPROBE).nacp $(UIPROBE).nro $(BUILD)/fex-uiprobe-host
	$(RM) $(BUILD)/DownloadProbe.o $(BUILD)/DownloadProbe.d $(DLPROBE).elf $(DLPROBE).nacp $(DLPROBE).nro
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

$(BUILD)/fex-hello.o: probes/HelloGuest.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/fex-hello.elf: $(BUILD)/fex-hello.o probes/HelloGuest.ld
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T probes/HelloGuest.ld $< -o $@

HOST_X86_CC ?= cc
# Use a hyphenated folder so the same targets work with make and on the SD card.
MAZE_DIR := $(BUILD)/sd-maze

.PHONY: maze-elf
maze-elf: $(MAZE_DIR)/sd-maze.elf $(MAZE_DIR)/sd-maze.elf.args $(MAZE_DIR)/data/maze.txt

$(BUILD)/maze-start.o: demos/Maze/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/maze.o: demos/Maze/Maze.c | $(BUILD)
	$(HOST_X86_CC) -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(MAZE_DIR)/sd-maze.elf: $(BUILD)/maze-start.o $(BUILD)/maze.o probes/HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T probes/HelloGuest.ld $(filter %.o,$^) -o $@

$(MAZE_DIR)/sd-maze.elf.args: demos/Maze/sd-maze.elf.args
	mkdir -p $(@D)
	cp $< $@

$(MAZE_DIR)/data/maze.txt: demos/Maze/data/maze.txt
	mkdir -p $(@D)
	cp $< $@

GRAPHICS_DIR := $(BUILD)/texture-demo

.PHONY: texture-demo
texture-demo: $(GRAPHICS_DIR)/texture-demo.elf $(GRAPHICS_DIR)/data/sprite.rgba

$(BUILD)/texture-start.o: demos/Graphics/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/texture-demo.o: demos/Graphics/TextureDemo.c GuestVideo.h | $(BUILD)
	$(HOST_X86_CC) -Iruntime -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(GRAPHICS_DIR)/texture-demo.elf: $(BUILD)/texture-start.o $(BUILD)/texture-demo.o probes/HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T probes/HelloGuest.ld $(filter %.o,$^) -o $@

$(GRAPHICS_DIR)/data/sprite.rgba: demos/Graphics/MakeTexture.py
	mkdir -p $(@D)
	python3 $< $@

SDL2_CFLAGS ?= $(shell pkg-config --cflags sdl2)
STAR_DIR := $(BUILD)/star-catch

.PHONY: star-catch
star-catch: $(STAR_DIR)/star-catch.elf $(STAR_DIR)/data/star.rgba

$(BUILD)/sdl2-start.o: demos/SDL2/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/sdl2-guest.o: demos/SDL2/SDL2.c demos/SDL2/GuestSyscall.h GuestVideo.h | $(BUILD)
	$(HOST_X86_CC) -Iruntime $(SDL2_CFLAGS) -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/star-catch.o: demos/SDL2/StarCatch.c | $(BUILD)
	$(HOST_X86_CC) $(SDL2_CFLAGS) -DSDL_MAIN_HANDLED -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror -c $< -o $@

$(STAR_DIR)/star-catch.elf: $(BUILD)/sdl2-start.o $(BUILD)/sdl2-guest.o $(BUILD)/star-catch.o probes/HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T probes/HelloGuest.ld $(filter %.o,$^) -o $@

$(STAR_DIR)/data/star.rgba: demos/Graphics/MakeTexture.py
	mkdir -p $(@D)
	python3 $< $@

DYNAMIC_DIR := $(BUILD)/dynamic-demo
DYNAMIC_CFLAGS := -m64 -Os -ffreestanding -fPIE -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -Werror

.PHONY: dynamic-demo
dynamic-demo: $(DYNAMIC_DIR)/games/dynamic-demo.elf $(DYNAMIC_DIR)/rootfs/lib/ld-fex-demo-x86-64.so.2

$(BUILD)/dynamic-start.o: demos/Dynamic/Start.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/dynamic-main.o: demos/Dynamic/Demo.S | $(BUILD)
	$(HOST_X86_AS) --64 $< -o $@

$(BUILD)/dynamic-loader.o: demos/Dynamic/Loader.c demos/SDL2/GuestSyscall.h | $(BUILD)
	$(HOST_X86_CC) $(DYNAMIC_CFLAGS) -c $< -o $@

$(BUILD)/dynamic-library.o: demos/Dynamic/Library.c demos/SDL2/GuestSyscall.h | $(BUILD)
	$(HOST_X86_CC) $(DYNAMIC_CFLAGS) -fPIC -c $< -o $@

$(DYNAMIC_DIR)/rootfs/lib/ld-fex-demo-x86-64.so.2: $(BUILD)/dynamic-start.o $(BUILD)/dynamic-loader.o probes/HelloGuest.ld
	mkdir -p $(@D)
	$(HOST_X86_LD) -m elf_x86_64 -pie --no-dynamic-linker -T probes/HelloGuest.ld $(filter %.o,$^) -o $@

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
	python3 tools/StageRootFS.py $(STOCK_DIR)/fex "$(STOCK_TRUE)" "$<" "$(HOST_X86_CC)" "$(STOCK_LIB_DIR)"

$(BUILD)/glibc-probe.elf: tests/guest/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -O2 -fPIE -pie -Wall -Wextra -Werror $< -ldl -o $@

$(BUILD)/glibc-probe-baseline.o: tests/guest/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/glibc-probe-baseline.elf: $(BUILD)/glibc-probe-baseline.o $(STOCK_CRT_DIR)/Scrt1.o $(STOCK_CRT_DIR)/crti.o $(STOCK_CRT_DIR)/crtn.o $(STOCK_LIB_DIR)/libc.so.6
	$(HOST_X86_LD) -m elf_x86_64 -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 -rpath-link "$(STOCK_LIB_DIR)" \
		"$(STOCK_CRT_DIR)/Scrt1.o" "$(STOCK_CRT_DIR)/crti.o" $< "$(STOCK_LIB_DIR)/libc.so.6" "$(STOCK_CRT_DIR)/crtn.o" -o $@

check-guest-arena: $(BUILD)/guest-arena-test
	$<

$(BUILD)/guest-arena-test: tests/unit/GuestArenaTest.cpp GuestArena.h GuestElf.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

check-guest-memory: $(BUILD)/guest-address-space-test $(BUILD)/guest-memory-file-test
	$(BUILD)/guest-address-space-test
	$(BUILD)/guest-memory-file-test

$(BUILD)/guest-memory-file-test: tests/unit/GuestMemoryFileTest.cpp GuestMemoryFile.h GuestFilePunch.h GuestAddressSpace.h GuestArena.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

$(BUILD)/guest-address-space-test: tests/unit/AddressSpaceTest.cpp GuestAddressSpace.h GuestArena.h GuestElf.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

# Populate with `make debian-sysroot` (Debian 12 amd64 packages).
DEBIAN_ROOT ?= $(abspath $(BUILD)/debian-sysroot)
DEBIAN_TEST_DIR ?= $(BUILD)/debian-runtime
DEBIAN_LIBS = $(DEBIAN_ROOT)/lib/x86_64-linux-gnu
DEBIAN_CRT = $(DEBIAN_ROOT)/usr/lib/x86_64-linux-gnu

.PHONY: debian-runtime
debian-runtime: $(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf $(BUILD)/process-probe-debian.elf
	python3 tools/StageDebian.py "$(DEBIAN_ROOT)" "$(DEBIAN_TEST_DIR)" $^

.PHONY: debian-sysroot
debian-sysroot:
	python3 -I tools/FetchDebian.py "$(DEBIAN_ROOT)"

FILESYSTEM_TEST_DIR ?= $(BUILD)/guest/sd
.PHONY: filesystem-runtime
# Stages only test programs, sidecars and helper scripts into
# $(FILESYSTEM_TEST_DIR)/fex/games: nothing that would overwrite the installed
# Ubuntu RootFS. The folder is removed first so stale files disappear.
filesystem-runtime: $(BUILD)/op-bench-debian.elf $(BUILD)/final-pass-suite-debian.elf $(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf $(BUILD)/process-probe-debian.elf $(BUILD)/filesystem-probe-debian.elf $(BUILD)/fs-suite-debian.elf $(BUILD)/name-limit-debian.elf $(BUILD)/proc-suite-debian.elf $(BUILD)/process-acceptance-debian.elf $(BUILD)/comm-suite-debian.elf $(BUILD)/comm-acceptance-debian.elf $(BUILD)/container-suite-debian.elf $(BUILD)/container-acceptance-debian.elf $(BUILD)/pty-suite-debian.elf $(BUILD)/thread-suite-debian.elf $(BUILD)/kernel-files-suite-debian.elf
	rm -rf "$(FILESYSTEM_TEST_DIR)/fex"
	python3 tools/StageDebian.py --games "$(DEBIAN_ROOT)" "$(FILESYSTEM_TEST_DIR)" $(wordlist 3,5,$^) --filesystem-probe $(word 6,$^) --fs-suite $(word 7,$^) --name-limit-probe $(word 8,$^) --proc-suite $(word 9,$^) --process-acceptance $(word 10,$^) --comm-suite $(word 11,$^) --comm-acceptance $(word 12,$^) --container-suite $(word 13,$^) --container-acceptance $(word 14,$^) --pty-suite $(word 15,$^) --thread-suite $(word 16,$^) --op-bench $(word 1,$^) --final-pass-suite $(word 2,$^) --kernel-files-suite $(word 17,$^)

# Filesystem compatibility suite: the Debian guest ELF and a native reference
# run on the host kernel, as the current user and, where user namespaces
# with subordinate IDs are available, as namespace root.
.PHONY: fs-suite check-fs-suite-native
fs-suite: $(BUILD)/fs-suite-debian.elf

$(BUILD)/fs-suite.o: tests/guest/FsSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

# Measures the backing filesystem's name and path length limits.
$(BUILD)/name-limit.o: tests/guest/NameLimitProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/fs-suite-host: tests/guest/FsSuite.c | $(BUILD)
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

$(BUILD)/proc-suite.o: tests/guest/ProcSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/process-acceptance.o: tests/guest/ProcessAcceptance.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/proc-suite-host: tests/guest/ProcSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

check-proc-suite-native: $(BUILD)/proc-suite-host
	mkdir -p $(BUILD)/proc-suite-native
	cd $(BUILD)/proc-suite-native && { ../proc-suite-host; test $$? -eq 42; }
	if unshare --map-root-user --map-auto true 2>/dev/null; then \
		cd $(BUILD)/proc-suite-native && { unshare --map-root-user --map-auto ../proc-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-auto is unavailable"; \
	fi
	@# The guest ELF is built with the host headers and runs on Debian 12's glibc; run it here on the host kernel with that
	@# glibc too, so differences between the two C libraries show up on Linux and not only on the console. (Only the procfs
	@# cases: the suite's exec cases re-run the ELF through the host's loader, which is not this setup.)
	if [ -x "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" ]; then \
		$(MAKE) $(BUILD)/proc-suite-debian.elf && \
		cd $(BUILD)/proc-suite-native && { "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" --library-path "$(DEBIAN_LIBS)" ../proc-suite-debian.elf procfs/; test $$? -eq 42; }; \
	else \
		echo "Skipping the Debian glibc run: no debian-sysroot (make debian-sysroot)"; \
	fi

.PHONY: kernel-files-suite check-kernel-files-suite-native
kernel-files-suite: $(BUILD)/kernel-files-suite-debian.elf

# Use Debian's libc headers so scanf and strto* bind to symbols its runtime exports.
$(BUILD)/kernel-files-suite.o: tests/guest/KernelFilesSuite.c Makefile $(DEBIAN_CRT)/Scrt1.o | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -pthread -Wall -Wextra -Werror \
		-isystem "$(DEBIAN_ROOT)/usr/include/x86_64-linux-gnu" -isystem "$(DEBIAN_ROOT)/usr/include" -c $< -o $@

$(BUILD)/kernel-files-suite-host: tests/guest/KernelFilesSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -pthread -Wall -Wextra -Werror $< -o $@

check-kernel-files-suite-native: $(BUILD)/kernel-files-suite-host
	mkdir -p $(BUILD)/kernel-files-suite-native
	cd $(BUILD)/kernel-files-suite-native && { ../kernel-files-suite-host; test $$? -eq 42; }
	if unshare --map-root-user true 2>/dev/null; then \
		cd $(BUILD)/kernel-files-suite-native && { unshare --map-root-user ../kernel-files-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-root-user is unavailable"; \
	fi
	if [ -x "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" ]; then \
		$(MAKE) $(BUILD)/kernel-files-suite-debian.elf && \
		cd $(BUILD)/kernel-files-suite-native && { "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" --library-path "$(DEBIAN_LIBS)" ../kernel-files-suite-debian.elf; test $$? -eq 42; }; \
	else \
		echo "Skipping the Debian glibc run: no debian-sysroot (make debian-sysroot)"; \
	fi

# Pseudo-terminal suite (UI task 8a): same layout as the process suite.
.PHONY: pty-suite check-pty-suite-native
pty-suite: $(BUILD)/pty-suite-debian.elf

$(BUILD)/pty-suite.o: tests/guest/PtySuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/pty-suite-host: tests/guest/PtySuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

check-pty-suite-native: $(BUILD)/pty-suite-host
	mkdir -p $(BUILD)/pty-suite-native
	cd $(BUILD)/pty-suite-native && { ../pty-suite-host; test $$? -eq 42; }
	if unshare --map-root-user --map-auto true 2>/dev/null; then \
		cd $(BUILD)/pty-suite-native && { unshare --map-root-user --map-auto ../pty-suite-host; test $$? -eq 42; }; \
	else \
		echo "Skipping the namespace-root run: unshare --map-auto is unavailable"; \
	fi
	@# The guest ELF is built with the host headers and runs on Debian 12's glibc; run it here on the host kernel with that
	@# glibc too, so differences between the two C libraries show up on Linux and not only on the console.
	if [ -x "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" ]; then \
		$(MAKE) $(BUILD)/pty-suite-debian.elf && \
		cd $(BUILD)/pty-suite-native && { "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" --library-path "$(DEBIAN_LIBS)" ../pty-suite-debian.elf; test $$? -eq 42; }; \
	else \
		echo "Skipping the Debian glibc run: no debian-sysroot (make debian-sysroot)"; \
	fi

# Multithreading suite (multithreading Task 1): same layout as the process suite.
.PHONY: thread-suite check-thread-suite-native
thread-suite: $(BUILD)/thread-suite-debian.elf

# EXPERIMENT (xz slowness): operation microbenchmark.
$(BUILD)/op-bench.o: tests/guest/OpBench.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

# Final pass of a milestone: runs every required suite (StageDebian.py FINAL_PASS_SUITES) and adds up the results.
$(BUILD)/final-pass-suite.o: tests/guest/FinalPassSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/thread-suite.o: tests/guest/ThreadSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/thread-suite-host: tests/guest/ThreadSuite.c | $(BUILD)
	$(HOST_X86_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

check-thread-suite-native: $(BUILD)/thread-suite-host
	mkdir -p $(BUILD)/thread-suite-native
	cd $(BUILD)/thread-suite-native && { ../thread-suite-host; test $$? -eq 42; }
	if [ -x "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" ]; then \
		$(MAKE) $(BUILD)/thread-suite-debian.elf && \
		cd $(BUILD)/thread-suite-native && { "$(DEBIAN_LIBS)/ld-linux-x86-64.so.2" --library-path "$(DEBIAN_LIBS)" ../thread-suite-debian.elf; test $$? -eq 42; }; \
	else \
		echo "Skipping the Debian glibc run: no debian-sysroot (make debian-sysroot)"; \
	fi

# Memory and communication suite (milestone 3): same layout as the process
# suite. The native reference needs no network beyond 127.0.0.1.
.PHONY: comm-suite check-comm-suite-native
comm-suite: $(BUILD)/comm-suite-debian.elf

$(BUILD)/comm-suite.o: tests/guest/CommSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/comm-acceptance.o: tests/guest/CommAcceptance.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/comm-suite-host: tests/guest/CommSuite.c | $(BUILD)
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

$(BUILD)/container-suite.o: tests/guest/ContainerSuite.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/container-acceptance.o: tests/guest/ContainerAcceptance.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -std=gnu11 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/container-suite-host: tests/guest/ContainerSuite.c | $(BUILD)
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

$(BUILD)/filesystem-probe.o: tests/guest/FilesystemProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/thread-ipc-debian.elf $(BUILD)/glibc-probe-debian.elf $(BUILD)/process-probe-debian.elf $(BUILD)/filesystem-probe-debian.elf $(BUILD)/fs-suite-debian.elf $(BUILD)/name-limit-debian.elf $(BUILD)/proc-suite-debian.elf $(BUILD)/process-acceptance-debian.elf $(BUILD)/comm-suite-debian.elf $(BUILD)/comm-acceptance-debian.elf $(BUILD)/container-suite-debian.elf $(BUILD)/container-acceptance-debian.elf $(BUILD)/pty-suite-debian.elf $(BUILD)/thread-suite-debian.elf $(BUILD)/op-bench-debian.elf $(BUILD)/final-pass-suite-debian.elf $(BUILD)/kernel-files-suite-debian.elf: $(BUILD)/%-debian.elf: $(BUILD)/%.o $(DEBIAN_CRT)/Scrt1.o $(DEBIAN_CRT)/crti.o $(DEBIAN_CRT)/crtn.o $(DEBIAN_LIBS)/libc.so.6
	$(HOST_X86_LD) -m elf_x86_64 -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 -rpath-link "$(DEBIAN_LIBS)" \
		"$(DEBIAN_CRT)/Scrt1.o" "$(DEBIAN_CRT)/crti.o" $< "$(DEBIAN_LIBS)/libc.so.6" "$(DEBIAN_CRT)/crtn.o" -o $@

$(BUILD)/thread-ipc.o: tests/guest/ThreadIpcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -pthread -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/glibc-probe.o: tests/guest/GlibcProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/process-probe.o: tests/guest/ProcessProbe.c | $(BUILD)
	$(HOST_X86_CC) -m64 -march=x86-64 -O2 -fPIE -Wall -Wextra -Werror -c $< -o $@

.PHONY: check-guest-log
check-guest-log: $(BUILD)/guest-log-buffer-test
	$<

$(BUILD)/guest-log-buffer-test: tests/unit/GuestLogBufferTest.cpp GuestLogBuffer.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-terminal
check-guest-terminal: $(BUILD)/guest-terminal-test
	$<

$(BUILD)/guest-terminal-test: tests/unit/GuestTerminalTest.cpp GuestTerminal.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-pty
check-guest-pty: $(BUILD)/guest-pty-test
	$<

$(BUILD)/guest-pty-test: tests/unit/GuestPtyTest.cpp runtime/process/GuestPty.h GuestTerminal.h GuestJobControl.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -UNDEBUG $(INC) $< -o $@

.PHONY: check-guest-ftp
check-guest-ftp: $(BUILD)/ftp-server-host $(BUILD)/ftp-stats-test
	python3 tests/unit/FtpTest.py --server $<
	$(BUILD)/ftp-stats-test

$(BUILD)/ftp-stats-test: tests/unit/FtpStatsTest.cpp tests/unit/FtpServerHost.cpp GuestFtp.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror -pthread $(INC) -Itests/unit $< -o $@

$(BUILD)/ftp-server-host: tests/unit/FtpServerHost.cpp GuestFtp.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-inode-store
check-guest-inode-store: $(BUILD)/guest-inode-store-test
	$<

$(BUILD)/guest-inode-store-test: tests/unit/GuestInodeStoreTest.cpp GuestInodeStore.h GuestFileExtents.h GuestPosixAcl.h GuestDirectoryRecovery.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-file-write
check-guest-file-write: $(BUILD)/guest-file-write-test
	$<

$(BUILD)/guest-file-write-test: tests/unit/GuestFileWriteTest.cpp GuestFileWrite.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-file-punch
check-guest-file-punch: $(BUILD)/guest-file-punch-test
	$<

$(BUILD)/guest-file-punch-test: tests/unit/GuestFilePunchTest.cpp GuestFilePunch.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-preempt
check-guest-preempt: $(BUILD)/guest-preempt-test
	$<

$(BUILD)/guest-preempt-test: tests/unit/GuestPreemptTest.cpp GuestPreempt.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-credentials
check-guest-credentials: $(BUILD)/guest-credentials-test
	$<

$(BUILD)/guest-credentials-test: tests/unit/GuestCredentialsTest.cpp GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-vma-list
check-guest-vma-list: $(BUILD)/guest-vma-list-test
	$<

$(BUILD)/guest-vma-list-test: tests/unit/GuestVmaListTest.cpp GuestVmaList.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-procfs
check-guest-procfs: $(BUILD)/guest-procfs-test
	$<

$(BUILD)/guest-procfs-test: tests/unit/GuestProcfsTest.cpp GuestProcfs.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-sysctl
check-guest-sysctl: $(BUILD)/guest-sysctl-test
	$<

$(BUILD)/guest-sysctl-test: tests/unit/GuestSysctlTest.cpp GuestSysctl.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-sysfs
check-guest-sysfs: $(BUILD)/guest-sysfs-test
	$<

$(BUILD)/guest-sysfs-test: tests/unit/GuestSysfsTest.cpp GuestSysfs.h GuestInputFeed.h GuestEvdev.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-evdev
check-guest-evdev: $(BUILD)/guest-evdev-test
	$<

$(BUILD)/guest-evdev-test: tests/unit/GuestEvdevTest.cpp GuestEvdev.h GuestPosixAcl.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-input-feed
check-guest-input-feed: $(BUILD)/guest-input-feed-test
	$<

$(BUILD)/guest-input-feed-test: tests/unit/GuestInputFeedTest.cpp GuestInputFeed.h GuestEvdev.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-ioctl
check-guest-ioctl: $(BUILD)/guest-ioctl-test
	$<

$(BUILD)/guest-ioctl-test: tests/unit/GuestIoctlTest.cpp GuestIoctl.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-kernel-tree
check-guest-kernel-tree: $(BUILD)/guest-kernel-tree-test
	$<

$(BUILD)/guest-kernel-tree-test: tests/unit/GuestKernelTreeTest.cpp GuestKernelTree.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-clocks
check-guest-clocks: $(BUILD)/guest-clocks-test
	$<

$(BUILD)/guest-clocks-test: tests/unit/GuestClocksTest.cpp GuestClocks.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-eventfd
check-guest-eventfd: $(BUILD)/guest-eventfd-test
	$<

$(BUILD)/guest-eventfd-test: tests/unit/GuestEventFdTest.cpp GuestEventFd.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-inet
check-guest-inet: $(BUILD)/guest-inet-test
	$<

$(BUILD)/guest-inet-test: tests/unit/GuestInetTest.cpp GuestInet.h GuestSocket.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-sched
check-guest-sched: $(BUILD)/guest-sched-test
	$<

$(BUILD)/guest-sched-test: tests/unit/GuestSchedTest.cpp GuestSched.h GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-namespaces
check-guest-namespaces: $(BUILD)/guest-namespaces-test
	$<

$(BUILD)/guest-namespaces-test: tests/unit/GuestNamespacesTest.cpp GuestNamespaces.h GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-mounts
check-guest-mounts: $(BUILD)/guest-mounts-test
	$<

$(BUILD)/guest-mounts-test: tests/unit/GuestMountsTest.cpp GuestMounts.h GuestNamespaces.h GuestCredentials.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-seccomp
check-guest-seccomp: $(BUILD)/guest-seccomp-test
	$<

$(BUILD)/guest-seccomp-test: tests/unit/GuestSeccompTest.cpp GuestSeccomp.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-futex
check-guest-futex: $(BUILD)/guest-futex-test
	$<

$(BUILD)/guest-futex-test: tests/unit/GuestFutexTest.cpp GuestFutex.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-pipe
check-guest-pipe: $(BUILD)/guest-pipe-packet-test
	$<

$(BUILD)/guest-pipe-packet-test: tests/unit/GuestPipePacketTest.cpp GuestSocket.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-socket
check-guest-socket: $(BUILD)/guest-socket-test
	$<

$(BUILD)/guest-socket-test: tests/unit/GuestSocketTest.cpp GuestSocket.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-sysv-ipc
check-guest-sysv-ipc: $(BUILD)/guest-sysv-ipc-test
	$<

$(BUILD)/guest-sysv-ipc-test: tests/unit/GuestSysvIpcTest.cpp GuestSysvIpc.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-mqueue
check-guest-mqueue: $(BUILD)/guest-mqueue-test
	$<

$(BUILD)/guest-mqueue-test: tests/unit/GuestMqueueTest.cpp GuestMqueue.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-signals
check-guest-signals: $(BUILD)/guest-signals-test
	$<

$(BUILD)/guest-signals-test: tests/unit/GuestSignalsTest.cpp GuestSignals.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-job-control
check-guest-job-control: $(BUILD)/guest-job-control-test
	$<

$(BUILD)/guest-job-control-test: tests/unit/GuestJobControlTest.cpp GuestJobControl.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-inotify
check-guest-inotify: $(BUILD)/guest-inotify-test
	$<

$(BUILD)/guest-inotify-test: tests/unit/GuestInotifyTest.cpp GuestInotify.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-epoll
check-guest-epoll: $(BUILD)/guest-epoll-test
	$<

$(BUILD)/guest-epoll-test: tests/unit/GuestEpollTest.cpp GuestEpoll.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-timers
check-guest-timers: $(BUILD)/guest-timers-test
	$<

$(BUILD)/guest-timers-test: tests/unit/GuestTimersTest.cpp GuestTimers.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

.PHONY: check-guest-script
check-guest-script: $(BUILD)/guest-script-test
	$<

$(BUILD)/guest-script-test: tests/unit/GuestScriptTest.cpp GuestScript.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $< -o $@

# Debian rootfs setup (launcher menu): unpacker checks against Python's
# ar/tar readers on every package of the manifest.
XZ_EMBEDDED := ThirdParty/xz-embedded
XZ_DEFINES := -DXZ_USE_CRC64 -DXZ_USE_SHA256 -DXZ_DEC_X86 -DXZ_DEC_CONCATENATED
XZ_SOURCES := $(addprefix $(XZ_EMBEDDED)/,xz_crc32.c xz_crc64.c xz_sha256.c xz_dec_stream.c xz_dec_lzma2.c xz_dec_bcj.c)

ZSTD_SOURCE := ThirdParty/zstd/zstddeclib.c
XXH_INC := -I$(ROOT)/External/xxhash

$(BUILD)/xz_%.o: $(XZ_EMBEDDED)/xz_%.c | $(BUILD)
	cc -std=gnu11 -O2 -I$(XZ_EMBEDDED) $(XZ_DEFINES) -c $< -o $@

$(BUILD)/zstddeclib.o: $(ZSTD_SOURCE) | $(BUILD)
	cc -std=gnu11 -O2 -c $< -o $@

XZ_OBJECTS := $(addprefix $(BUILD)/,$(notdir $(XZ_SOURCES:.c=.o)))

# RootFS image installer (squashfs reader, unbreak step, extras, pipeline), checked against host mksquashfs and unsquashfs.
$(BUILD)/guest-rootfs-install-test: tests/unit/GuestRootfsInstallTest.cpp GuestRootfsInstall.h GuestSquashfs.h GuestRootfsSetup.h OpenPgp.h UbuntuArchiveKey.h HttpClient.h $(XZ_OBJECTS) $(BUILD)/zstddeclib.o | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $(INC) $(XXH_INC) $(XZ_DEFINES) -c tests/unit/GuestRootfsInstallTest.cpp -o $@.o
	$(HOST_CXX) $@.o $(XZ_OBJECTS) $(BUILD)/zstddeclib.o -o $@

.PHONY: check-rootfs-install
check-rootfs-install: $(BUILD)/guest-rootfs-install-test tools/CheckRootfsInstall.py
	python3 -I tools/CheckRootfsInstall.py $(BUILD)/guest-rootfs-install-test $(BUILD)/rootfs-install-check

# The NRO is the only file the user copies: every font, icon, license text, licenses.json and the RootFS index must be inside it.
# Needs the launcher tree: cmake --build build/guest --target guest_nro
.PHONY: check-nro-assets
check-nro-assets:
	python3 -I tools/CheckNroAssets.py $(BUILD)/guest/fex-switch2-guest.nro $(UI_DIR)/assets
