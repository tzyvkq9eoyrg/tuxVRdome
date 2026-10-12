cmake_minimum_required(VERSION 3.16)
project(tuxvrdome VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL)
find_package(Vulkan REQUIRED)

add_executable(tuxvrdome
  src/main.cpp
  src/xr_context.cpp
  src/video_player.cpp
  src/gl_util.cpp
  src/cairo_canvas.cpp
  src/file_ops.cpp
  src/file_util.cpp
  src/frame_analysis.cpp
  src/frame_grabber.cpp
  src/menu.cpp
  src/photo_preloader.cpp
  src/renderer.cpp
  src/favorites.cpp
  src/settings.cpp
  src/stream_format.cpp
  src/strings.cpp
  src/subtitle_overlay.cpp
  src/thumbnailer.cpp
  src/video_format.cpp
  src/video_memory.cpp
  src/vr_space.cpp
  src/youtube.cpp
)

# Checks of the headset-free logic (format detection, YouTube rules, links);
# build.sh runs them after each build.
add_executable(tuxvrdome_tests
  tests/tests.cpp
  src/file_util.cpp
  src/stream_format.cpp
  src/video_format.cpp
  src/youtube.cpp
)
target_link_libraries(tuxvrdome_tests PRIVATE
  PkgConfig::CURL
  PkgConfig::PIXBUF
  nlohmann_json::nlohmann_json
  Threads::Threads
)

target_compile_definitions(tuxvrdome PRIVATE GL_GLEXT_PROTOTYPES
                                             TUXVRDOME_VERSION="${PROJECT_VERSION}")
# OpenXR structs are initialized as {XR_TYPE_...}, leaving the rest zeroed on purpose.
target_compile_options(tuxvrdome PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(tuxvrdome PRIVATE
  PkgConfig::OPENXR
  PkgConfig::MPV
  PkgConfig::GLFW
  PkgConfig::PANGOCAIRO
  PkgConfig::CURL
  PkgConfig::PIXBUF
  PkgConfig::GIO
  PkgConfig::FFMPEG
  nlohmann_json::nlohmann_json
  Threads::Threads
  OpenGL::OpenGL
  Vulkan::Vulkan
)

# Installation (also what the AppImage is made from): the program, a menu
# entry and its icon.
include(GNUInstallDirs)
install(TARGETS tuxvrdome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/tuxvrdome.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/tuxvrdome.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
