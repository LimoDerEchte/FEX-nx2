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

add_library(fex_switch2_host OBJECT HostMemory.cpp)
target_link_libraries(fex_switch2_host PRIVATE FEXCore_Base)

# Use the devkit driver for its startup objects, newlib, libstdc++ and Switch specs.
set(CMAKE_CXX_LINK_EXECUTABLE
  "${CMAKE_COMMAND} -E env DEVKITPRO=${DKP} ${DKP_CXX} ${DKP_CPU} -mtp=soft -fPIE -specs=${DKP}/libounce/switch.specs <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
add_executable(fex-switch2-guest GuestProbe.cpp GuestRuntime.cpp GuestElf.cpp JitProbe.cpp)
target_compile_definitions(fex-switch2-guest PRIVATE FEX_SWITCH2_LAUNCHER=1)
target_include_directories(fex-switch2-guest PRIVATE "${CMAKE_BINARY_DIR}/include")
target_link_directories(fex-switch2-guest PRIVATE "${DKP}/portlibs/switch2/lib" "${DKP}/libounce/lib")
target_link_libraries(fex-switch2-guest PRIVATE fex_switch2_host FEXCore FEXCore_Base JemallocLibs kaihen nx)
set_target_properties(fex-switch2-guest PROPERTIES SUFFIX ".elf")

set(GUEST_NACP "${CMAKE_CURRENT_BINARY_DIR}/fex-switch2-guest.nacp")
set(GUEST_NRO "${CMAKE_CURRENT_BINARY_DIR}/fex-switch2-guest.nro")
set(GUEST_ICON "${DKP}/libounce/default_icon.jpg")
add_custom_command(OUTPUT "${GUEST_NACP}"
  COMMAND "${DKP}/tools/bin/nacptool" --create "FEX for Switch 2" "FEX Switch 2 fork" "0.11.5" "${GUEST_NACP}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
  VERBATIM)
add_custom_command(OUTPUT "${GUEST_NRO}"
  COMMAND "${DKP}/tools/bin/elf2nro" $<TARGET_FILE:fex-switch2-guest> "${GUEST_NRO}"
    "--icon=${GUEST_ICON}" "--nacp=${GUEST_NACP}"
  DEPENDS fex-switch2-guest "${GUEST_NACP}" "${GUEST_ICON}"
  VERBATIM)
add_custom_target(guest_nro ALL DEPENDS "${GUEST_NRO}")
