# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.21)
project(FEXSwitch2 LANGUAGES C CXX)
include(CheckCXXSourceCompiles)

if(NOT CMAKE_CROSSCOMPILING OR NOT DEFINED DKP_CXX)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=Toolchain.cmake")
endif()

set(FEX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(ARCHITECTURE_arm64 ON)
set(FEX_SWITCH2 ON)
set(BUILD_TESTING OFF)
set(BUILD_STEAM_SUPPORT ON)
set(CMAKE_INSTALL_FULL_LIBDIR "sdmc:/fex/lib")
set(CMAKE_INSTALL_PREFIX "sdmc:/fex")
set(GIT_DESCRIBE "switch2")
set(GIT_HASH "switch2")

add_compile_definitions(__SWITCH__ __SWITCH2__ ARCHITECTURE_arm64=1 FEX_DISABLE_TELEMETRY=1
  GLOBAL_DATA_DIRECTORY="sdmc:/fex/")
add_compile_options(-ffunction-sections -fdata-sections)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

add_library(fmt_headers INTERFACE)
if(EXISTS "${FEX_ROOT}/External/fmt/include/fmt/format.h")
  target_include_directories(fmt_headers SYSTEM INTERFACE "${FEX_ROOT}/External/fmt/include")
else()
  find_path(FEX_FMT_INCLUDE fmt/format.h REQUIRED)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/host-headers")
  file(CREATE_LINK "${FEX_FMT_INCLUDE}/fmt" "${CMAKE_BINARY_DIR}/host-headers/fmt" SYMBOLIC COPY_ON_ERROR)
  target_include_directories(fmt_headers SYSTEM INTERFACE "${CMAKE_BINARY_DIR}/host-headers")
endif()
target_compile_definitions(fmt_headers INTERFACE FMT_HEADER_ONLY)
add_library(fmt::fmt ALIAS fmt_headers)

add_library(xxhash_headers INTERFACE)
if(EXISTS "${FEX_ROOT}/External/xxhash/xxhash.h")
  target_include_directories(xxhash_headers SYSTEM INTERFACE "${FEX_ROOT}/External/xxhash")
else()
  find_path(FEX_XXHASH_INCLUDE xxhash.h REQUIRED)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/host-hash")
  file(CREATE_LINK "${FEX_XXHASH_INCLUDE}/xxhash.h" "${CMAKE_BINARY_DIR}/host-hash/xxhash.h" SYMBOLIC COPY_ON_ERROR)
  target_include_directories(xxhash_headers SYSTEM INTERFACE "${CMAKE_BINARY_DIR}/host-hash")
endif()
target_compile_definitions(xxhash_headers INTERFACE XXH_INLINE_ALL)
add_library(xxHash::xxhash ALIAS xxhash_headers)

function(LinkerGC Target)
  target_link_options(${Target} PRIVATE LINKER:--gc-sections)
endfunction()

add_subdirectory("${FEX_ROOT}/External/unordered_dense" unordered_dense)
add_subdirectory("${FEX_ROOT}/External/SoftFloat-3e" softfloat)
add_subdirectory("${FEX_ROOT}/External/cephes" cephes)
add_subdirectory("${FEX_ROOT}/FEXHeaderUtils" headerutils)
add_subdirectory("${FEX_ROOT}/CodeEmitter" codeemitter)
add_subdirectory("${FEX_ROOT}/FEXCore" fexcore)

add_library(fex_switch2_host OBJECT host/HostMemory.cpp)
target_link_libraries(fex_switch2_host PRIVATE FEXCore_Base)

# Use the devkit driver for its startup objects, newlib, libstdc++ and Switch specs.
set(CMAKE_CXX_LINK_EXECUTABLE
  "${CMAKE_COMMAND} -E env DEVKITPRO=${DKP} ${DKP_CXX} ${DKP_CPU} -mtp=soft -fPIE -specs=${DKP}/libounce/switch.specs <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
# XZ Embedded (0BSD) decodes Debian package members for the rootfs setup.
set(XZ_EMBEDDED "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/xz-embedded")
add_library(xz_embedded OBJECT "${XZ_EMBEDDED}/xz_crc32.c" "${XZ_EMBEDDED}/xz_crc64.c" "${XZ_EMBEDDED}/xz_sha256.c"
  "${XZ_EMBEDDED}/xz_dec_stream.c" "${XZ_EMBEDDED}/xz_dec_lzma2.c" "${XZ_EMBEDDED}/xz_dec_bcj.c")
target_include_directories(xz_embedded PRIVATE "${XZ_EMBEDDED}")
target_compile_definitions(xz_embedded PUBLIC XZ_USE_CRC64 XZ_USE_SHA256 XZ_DEC_X86 XZ_DEC_CONCATENATED)
# The zstd decoder (BSD, ThirdParty/zstd) decodes the RootFS squashfs image and Ubuntu package members; vendored sources are not ours to warn on.
set(ZSTD_DECODER "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/zstd/zstddeclib.c")
set_source_files_properties("${ZSTD_DECODER}" PROPERTIES COMPILE_OPTIONS "-w")
# The RootFS index is embedded in the NRO (ui/assets/rootfs-index.json); the installer's index parser uses the UI's small JSON tree.
# The UI (ui/): the Vulkan platform and renderer, fonts, icons, theme, input, components and screens, the app shell. Shaders are
# compiled to SPIR-V headers like the Makefile does; fonts, icons, licenses and the RootFS index are linked into the NRO (.incbin).
set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ui")
set(UI_GEN "${CMAKE_BINARY_DIR}/ui-gen")
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
foreach(Stage vert frag)
  add_custom_command(OUTPUT "${UI_GEN}/ui_${Stage}.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UI_GEN}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --quiet --vn ui_${Stage} -o "${UI_GEN}/ui_${Stage}.h" "${UI_DIR}/render/shaders/ui.${Stage}"
    DEPENDS "${UI_DIR}/render/shaders/ui.${Stage}" VERBATIM)
endforeach()
set(UI_SOURCES
  platform/common/Log platform/common/Names render/vulkan/Target render/vulkan/VulkanRenderer
  render/Atlas render/Fonts render/Icons assets/Assets assets/IndexAsset theme theme_dark theme_light
  input/InputModes input/Focus input/Interaction components/Basic components/Chrome components/Controls components/Modals components/Progress
  components/Rows components/LicenseModal Storage PathUtil Json RootfsIndex SettingsSchema LicenseStore FileBrowser screens/home screens/settings
  screens/filepicker components/Tiles components/Terminal TerminalGrid TerminalKeys FtpModel InstallModel RootfsSelection screens/ftp screens/rootfs_setup
  screens/install screens/shell App RootfsState platform/switch2/Switch2Platform platform/switch2/Switch2AppInit)
list(TRANSFORM UI_SOURCES PREPEND "${UI_DIR}/")
list(TRANSFORM UI_SOURCES APPEND ".cpp")
set(UI_SHADER_HEADERS "${UI_GEN}/ui_vert.h" "${UI_GEN}/ui_frag.h")
file(GLOB_RECURSE UI_ASSET_FILES "${UI_DIR}/assets/*.json" "${UI_DIR}/assets/*.ttf" "${UI_DIR}/assets/*.svg" "${UI_DIR}/assets/*.txt")
# The FEX version of the build (git describe FEX-<yymm>-...), shown on Home.
execute_process(COMMAND git -C "${FEX_ROOT}" describe --tags --match "FEX-*" OUTPUT_VARIABLE FEX_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(REGEX MATCH "^FEX-([0-9]+)" FEX_DESCRIBE_MATCH "${FEX_DESCRIBE}")
if(CMAKE_MATCH_1)
  set(FEX_BUILD_NUMBER "${CMAKE_MATCH_1}")
else()
  set(FEX_BUILD_NUMBER "unknown")
endif()

add_executable(fex-switch2-guest launcher/GuestProbe.cpp launcher/LauncherBackend.cpp runtime/GuestRuntime.cpp runtime/GuestElf.cpp host/HostArena.cpp host/HostNet.cpp
  launcher/RootfsInstallHost.cpp ${UI_SOURCES} "${ZSTD_DECODER}" ${UI_SHADER_HEADERS}
  $<TARGET_OBJECTS:xz_embedded>)
set_source_files_properties(ui/assets/IndexAsset.cpp ui/RootfsIndex.cpp PROPERTIES COMPILE_DEFINITIONS "UI_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/ui/assets\"")
set_source_files_properties(ui/assets/IndexAsset.cpp ui/assets/Assets.cpp PROPERTIES COMPILE_DEFINITIONS "UI_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/ui/assets\"")
set_source_files_properties(ui/assets/IndexAsset.cpp ui/assets/Assets.cpp PROPERTIES OBJECT_DEPENDS "${UI_ASSET_FILES}")
target_compile_definitions(fex-switch2-guest PRIVATE FEX_SWITCH2_FEX_BUILD="${FEX_BUILD_NUMBER}")
target_compile_definitions(fex-switch2-guest PRIVATE XZ_USE_CRC64 XZ_USE_SHA256 XZ_DEC_X86 XZ_DEC_CONCATENATED)
target_compile_definitions(fex-switch2-guest PRIVATE FEX_SWITCH2_LAUNCHER=1)
# Include strategy: no relative includes; every source folder with headers is on the include path.
foreach(Dir runtime runtime/fs runtime/process runtime/memory runtime/ipc host .)
  target_include_directories(fex-switch2-guest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${Dir}")
endforeach()
target_include_directories(fex-switch2-guest PRIVATE "${CMAKE_BINARY_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${UI_GEN}" launcher
  "${DKP}/portlibs/switch/include/freetype2" "${DKP}/portlibs/switch/include/harfbuzz")
target_link_directories(fex-switch2-guest PRIVATE "${DKP}/portlibs/switch2/lib" "${DKP}/portlibs/switch/lib" "${DKP}/libounce/lib")
target_link_libraries(fex-switch2-guest PRIVATE fex_switch2_host FEXCore FEXCore_Base JemallocLibs xxHash::xxhash kaihen vulkan harfbuzz freetype png16 bz2 z m nx)
# abort() goes through __wrap_abort (launcher/GuestProbe.cpp), which logs before aborting.
target_link_options(fex-switch2-guest PRIVATE LINKER:--wrap=abort)
set_target_properties(fex-switch2-guest PROPERTIES SUFFIX ".elf")

set(GUEST_NACP "${CMAKE_CURRENT_BINARY_DIR}/fex-switch2-guest.nacp")
set(GUEST_NRO "${CMAKE_CURRENT_BINARY_DIR}/fex-switch2-guest.nro")
set(GUEST_ICON "${CMAKE_CURRENT_SOURCE_DIR}/launcher/FexIcon.jpg")
add_custom_command(OUTPUT "${GUEST_NACP}"
  COMMAND "${DKP}/tools/bin/nacptool" --create "FEX for Switch 2" "FEX Team, Limo (nx2)" "0.18.18" "${GUEST_NACP}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
  VERBATIM)
add_custom_command(OUTPUT "${GUEST_NRO}"
  COMMAND "${DKP}/tools/bin/elf2nro" $<TARGET_FILE:fex-switch2-guest> "${GUEST_NRO}"
    "--icon=${GUEST_ICON}" "--nacp=${GUEST_NACP}"
  DEPENDS fex-switch2-guest "${GUEST_NACP}" "${GUEST_ICON}"
  VERBATIM)
add_custom_target(guest_nro ALL DEPENDS "${GUEST_NRO}")

# EXPERIMENT (multithreading milestone, Task 0): the multithreading probe NRO (`make mt-probe`; log sdmc:/fex/mtprobe.log).
# Its x86 test code (probes/MultithreadBlobs.S) is assembled with the build machine's x86-64 `as` into a generated header.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MT_GEN "${CMAKE_BINARY_DIR}/mtprobe-gen")
add_custom_command(OUTPUT "${MT_GEN}/MtBlobs.h"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/probes/MakeMultithreadBlobs.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/probes/MultithreadBlobs.S" "${MT_GEN}" "${MT_GEN}/MtBlobs.h"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/probes/MultithreadBlobs.S" "${CMAKE_CURRENT_SOURCE_DIR}/probes/MakeMultithreadBlobs.py"
  VERBATIM)
add_executable(fex-switch2-mtprobe probes/MultithreadProbe.cpp host/HostArena.cpp "${MT_GEN}/MtBlobs.h")
target_include_directories(fex-switch2-mtprobe PRIVATE "${MT_GEN}" "${CMAKE_BINARY_DIR}/include")
foreach(Dir runtime runtime/fs runtime/process runtime/memory runtime/ipc host .)
  target_include_directories(fex-switch2-mtprobe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${Dir}")
endforeach()
target_link_directories(fex-switch2-mtprobe PRIVATE "${DKP}/portlibs/switch2/lib" "${DKP}/portlibs/switch/lib" "${DKP}/libounce/lib")
target_link_libraries(fex-switch2-mtprobe PRIVATE fex_switch2_host FEXCore FEXCore_Base JemallocLibs xxHash::xxhash kaihen m nx)
set_target_properties(fex-switch2-mtprobe PROPERTIES SUFFIX ".elf")
set(MT_NACP "${CMAKE_CURRENT_BINARY_DIR}/fex-switch2-mtprobe.nacp")
set(MT_NRO "${CMAKE_CURRENT_BINARY_DIR}/fex-switch2-mtprobe.nro")
add_custom_command(OUTPUT "${MT_NACP}"
  COMMAND "${DKP}/tools/bin/nacptool" --create "FEX Switch 2 MT Probe" "FEX Team, Limo (nx2)" "mtprobe 5" "${MT_NACP}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" VERBATIM)
add_custom_command(OUTPUT "${MT_NRO}"
  COMMAND "${DKP}/tools/bin/elf2nro" $<TARGET_FILE:fex-switch2-mtprobe> "${MT_NRO}" "--icon=${GUEST_ICON}" "--nacp=${MT_NACP}"
  DEPENDS fex-switch2-mtprobe "${MT_NACP}" "${GUEST_ICON}" VERBATIM)
add_custom_target(mtprobe_nro DEPENDS "${MT_NRO}")
