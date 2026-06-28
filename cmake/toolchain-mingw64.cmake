# CMake toolchain for cross-compiling dlm to 64-bit Windows with MinGW-w64.
#
# Primary supported Windows build is native, inside the MSYS2 MinGW64 shell
# (see Docs/building.md). This file additionally lets you cross-build from Linux
# IF you have the MinGW-w64 builds of the dependencies (curl, jansson, sqlite3,
# pcre2, openssl, and — for the GUI — gtk4/libadwaita) available to pkg-config.
#
#   cmake -S . -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win -j

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Let pkg-config resolve the MinGW sysroot packages.
set(ENV{PKG_CONFIG_LIBDIR} /usr/${TOOLCHAIN_PREFIX}/lib/pkgconfig)
set(ENV{PKG_CONFIG_SYSROOT_DIR} /usr/${TOOLCHAIN_PREFIX})
