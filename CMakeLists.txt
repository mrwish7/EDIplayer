cmake_minimum_required(VERSION 3.16)

# MSVC: link the C/C++ runtime statically so EDIplayer.exe runs without the
# Visual C++ Redistributable being installed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)   # same for GLFW

project(EDIplayer C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)   # the decoder ports (psram_unique_ptr.hpp) use concepts
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(EDIPLAYER_BUILD_GUI "Build the EDIplayer GUI" ON)
option(EDIPLAYER_BUILD_TOOLS "Build the edi-probe command line test tool" ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /bigobj)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS _USE_MATH_DEFINES NOMINMAX)
else()
  add_compile_options(-Wall)
endif()
if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0600)
endif()

# ---- EDI / FIC / PAD decoding (C, from edi-meta-forwarder) ----
add_library(edicore STATIC
  src/edi/crc16.c src/edi/pft.c src/edi/tags.c src/edi/fic.c
  src/edi/audio.c src/edi/pad.c src/edi/dls.c src/edi/mot.c)
target_include_directories(edicore PUBLIC src)

# ---- audio decoders (libfaad + libmad ports, from UecpBridge) ----
add_library(decoders STATIC
  src/decoders/aac_decoder/libfaad/neaacdec.cpp
  src/decoders/mp2_decoder/mp2_decoder.cpp
  src/decoders/mp2_decoder/libmad/bit.cpp
  src/decoders/mp2_decoder/libmad/fixed.cpp
  src/decoders/mp2_decoder/libmad/frame.cpp
  src/decoders/mp2_decoder/libmad/layer12.cpp
  src/decoders/mp2_decoder/libmad/stream.cpp
  src/decoders/mp2_decoder/libmad/synth.cpp
  src/decoders/mp2_decoder/libmad/timer.cpp)
target_include_directories(decoders PUBLIC src/decoders src)
# Enables SBR + PS in aac_settings.h (an ESP32-target-named guard in the port)
target_compile_definitions(decoders PUBLIC CONFIG_IDF_TARGET_ESP32S3)
if(MSVC)
  target_compile_options(decoders PRIVATE /wd4244 /wd4267 /wd4305 /wd4996 /wd4146)
else()
  target_compile_options(decoders PRIVATE -Wno-register -Wno-format-security -Wno-unused-function
                                          -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

# ---- player engine: network input, decoding pipeline, audio output ----
add_library(playercore STATIC
  src/core/log.cpp src/core/edi_source.cpp src/core/audio_decoders.cpp
  src/core/player.cpp src/core/audio_out.cpp)
target_include_directories(playercore PUBLIC src PRIVATE third_party/miniaudio)
target_link_libraries(playercore PUBLIC edicore decoders)
if(WIN32)
  target_link_libraries(playercore PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(playercore PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ---- GUI (Dear ImGui + GLFW + OpenGL 3) ----
if(EDIPLAYER_BUILD_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)   # X11 (works under XWayland too)
  add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
  find_package(OpenGL REQUIRED)

  set(IMGUI_DIR third_party/imgui)
  add_executable(EDIplayer WIN32
    src/gui/main.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(EDIplayer PRIVATE ${IMGUI_DIR} third_party/stb)
  target_link_libraries(EDIplayer PRIVATE playercore glfw OpenGL::GL)
  if(MSVC)
    # GUI subsystem, but with the standard main() entry point
    target_link_options(EDIplayer PRIVATE /ENTRY:mainCRTStartup)
  endif()
  install(TARGETS EDIplayer RUNTIME DESTINATION bin)
endif()

if(EDIPLAYER_BUILD_TOOLS)
  add_executable(edi-probe tools/edi_probe.cpp)
  target_link_libraries(edi-probe PRIVATE playercore)
endif()
