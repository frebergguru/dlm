cmake_minimum_required(VERSION 3.16)
project(dlm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Optional sanitizer build: -DDLM_ASAN=ON
option(DLM_ASAN "Build with AddressSanitizer" OFF)
if(DLM_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

# ---- libdlm (core engine) ------------------------------------------------
add_library(dlm STATIC
  libdlm/util.c
  libdlm/download.c
  libdlm/store.c
  libdlm/httpget.c
  libdlm/iaauth.c
  libdlm/verify.c
  libdlm/extract.c
  libdlm/extractors/archiveorg.c
  libdlm/extractors/ytdlp.c
)
target_include_directories(dlm PUBLIC libdlm/include)
target_include_directories(dlm PRIVATE
  libdlm
  ${CURL_INCLUDE_DIRS} ${JANSSON_INCLUDE_DIRS}
  ${SQLITE3_INCLUDE_DIRS} ${PCRE2_INCLUDE_DIRS} ${CRYPTO_INCLUDE_DIRS})
target_link_libraries(dlm PUBLIC
  ${CURL_LIBRARIES} ${JANSSON_LIBRARIES}
  ${SQLITE3_LIBRARIES} ${PCRE2_LIBRARIES} ${CRYPTO_LIBRARIES})

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
if(TARGET dlm-gui)
  install(TARGETS dlm-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  # desktop entry + icon so the GUI shows up in application menus
  install(FILES packaging/dlm.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/dlm.svg
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
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
