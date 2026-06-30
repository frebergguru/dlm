cmake_minimum_required(VERSION 3.16)
project(dlm VERSION 0.6.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build. build.sh already passes Release explicitly;
# this only affects a bare `cmake -B build` (which previously produced an
# unoptimized -O0 Debug binary).
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Optional sanitizer build: -DDLM_ASAN=ON
option(DLM_ASAN "Build with AddressSanitizer" OFF)
if(DLM_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- size/speed codegen (skipped under ASAN, which wants its own flags) ----
# These are portable GCC/Clang/MinGW flags only; nothing CPU-specific is enabled
# by default so the release artifacts (AppImage + Windows) stay portable. Tuning
# for the build host is opt-in via -DDLM_NATIVE=ON.
if(NOT DLM_ASAN)
  # Place each function/datum in its own section and let the linker drop the
  # ones nothing references -> smaller binaries and lower resident memory.
  include(CheckCCompilerFlag)
  check_c_compiler_flag("-ffunction-sections" DLM_HAS_FUNCTION_SECTIONS)
  if(DLM_HAS_FUNCTION_SECTIONS)
    add_compile_options(-ffunction-sections -fdata-sections)
    if(APPLE)
      add_link_options(-Wl,-dead_strip)
    else()
      add_link_options(-Wl,--gc-sections)
    endif()
  endif()

  # Link-time optimization for optimized configs. Verified available before use
  # so a toolchain without LTO support just builds normally.
  include(CheckIPOSupported)
  check_ipo_supported(RESULT DLM_IPO_OK OUTPUT DLM_IPO_MSG)
  if(DLM_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  else()
    message(STATUS "LTO not available: ${DLM_IPO_MSG}")
  endif()

  # Strip symbols from non-debug builds (smaller on-disk + mapped size).
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_link_options(-s)
  endif()

  # Opt-in host tuning (NOT for distributable binaries).
  option(DLM_NATIVE "Tune codegen for the build host (-march=native)" OFF)
  if(DLM_NATIVE)
    check_c_compiler_flag("-march=native" DLM_HAS_MARCH_NATIVE)
    if(DLM_HAS_MARCH_NATIVE)
      add_compile_options(-march=native)
    endif()
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- libdlm (core engine) ------------------------------------------------
add_library(dlm STATIC
  libdlm/util.c
  libdlm/download.c
  libdlm/store.c
  libdlm/httpget.c
  libdlm/iaauth.c
  libdlm/verify.c
  libdlm/tools.c
  libdlm/extract.c
  libdlm/extractors/archiveorg.c
  libdlm/extractors/ytdlp.c
  libdlm/compat/dlm_paths.c
  libdlm/compat/dlm_misc.c
  libdlm/compat/dlm_io.c
  libdlm/compat/dlm_sock.c
  libdlm/compat/dlm_proc.c
)
# libdlm is PUBLIC so consumers (dlmd/cli/gui) can include "compat/compat.h".
target_include_directories(dlm PUBLIC libdlm/include libdlm)
target_include_directories(dlm PRIVATE
  ${CURL_INCLUDE_DIRS} ${JANSSON_INCLUDE_DIRS}
  ${SQLITE3_INCLUDE_DIRS} ${PCRE2_INCLUDE_DIRS} ${CRYPTO_INCLUDE_DIRS})
target_link_libraries(dlm PUBLIC
  ${CURL_LIBRARIES} ${JANSSON_LIBRARIES}
  ${SQLITE3_LIBRARIES} ${PCRE2_LIBRARIES} ${CRYPTO_LIBRARIES}
  Threads::Threads)

# 64-bit off_t on 32-bit POSIX so pwrite/ftruncate handle >2GB files correctly.
if(NOT WIN32)
  target_compile_definitions(dlm PUBLIC _FILE_OFFSET_BITS=64)
endif()

if(WIN32)
  # Win10+ for AF_UNIX sockets + WSAPoll; link Winsock and shell helpers.
  # __USE_MINGW_ANSI_STDIO makes printf honour C99 %zu/%lld (MSVCRT does not).
  target_compile_definitions(dlm PUBLIC
    _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN __USE_MINGW_ANSI_STDIO=1)
  target_link_libraries(dlm PUBLIC ws2_32 shlwapi)
endif()

# ---- dlmd daemon ---------------------------------------------------------
add_executable(dlmd
  daemon/dlmd.c
  daemon/queue.c
  daemon/ipc.c
)
target_include_directories(dlmd PRIVATE
  daemon libdlm/include ${JANSSON_INCLUDE_DIRS})
target_link_libraries(dlmd PRIVATE dlm ${JANSSON_LIBRARIES} pthread)

# ---- dlm CLI -------------------------------------------------------------
add_executable(dlm-cli cli/dlm.c cli/client.c)
set_target_properties(dlm-cli PROPERTIES OUTPUT_NAME dlm)
target_include_directories(dlm-cli PRIVATE
  cli libdlm/include ${JANSSON_INCLUDE_DIRS})
target_link_libraries(dlm-cli PRIVATE dlm ${JANSSON_LIBRARIES})

# ---- dlm-gui (GTK4 + libadwaita) -----------------------------------------
option(DLM_GUI "Build the GTK4/libadwaita GUI" ON)
if(DLM_GUI)
  pkg_check_modules(GTK4 gtk4)
  pkg_check_modules(ADW libadwaita-1)
  if(GTK4_FOUND AND ADW_FOUND)
    add_executable(dlm-gui gui/main.c cli/client.c)
    target_include_directories(dlm-gui PRIVATE
      cli libdlm/include
      ${GTK4_INCLUDE_DIRS} ${ADW_INCLUDE_DIRS} ${JANSSON_INCLUDE_DIRS})
    target_link_libraries(dlm-gui PRIVATE dlm
      ${GTK4_LIBRARIES} ${ADW_LIBRARIES} ${JANSSON_LIBRARIES})
    message(STATUS "dlm-gui: enabled (GTK ${GTK4_VERSION}, libadwaita ${ADW_VERSION})")
  else()
    message(STATUS "dlm-gui: disabled (GTK4/libadwaita not found)")
  endif()
endif()

# ---- install -------------------------------------------------------------
include(GNUInstallDirs)

# binaries: dlm (CLI), dlmd (daemon), and dlm-gui when built
install(TARGETS dlm-cli dlmd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# license (GPL-3.0-or-later)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dlm)
if(TARGET dlm-gui)
  install(TARGETS dlm-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  # desktop entry + the full hicolor icon set (guru.freberg.dlm.*) so the GUI
  # shows up in application menus with a crisp icon at every size.
  install(FILES packaging/dlm.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(DIRECTORY assets/linux/hicolor
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
endif()

# optional systemd *user* service for dlmd (the daemon also auto-starts on
# first client use, so this is a convenience): -DDLM_INSTALL_SYSTEMD=ON
option(DLM_INSTALL_SYSTEMD "Install a systemd user service for dlmd" OFF)
if(DLM_INSTALL_SYSTEMD)
  configure_file(packaging/dlm.service.in
                 ${CMAKE_BINARY_DIR}/dlm.service @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/dlm.service
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
endif()

# ---- uninstall (cmake --build build --target uninstall) ------------------
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/cmake_uninstall.cmake.in"
    "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake" IMMEDIATE @ONLY)
  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake)
endif()

# ---- tests ---------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
