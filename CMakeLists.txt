cmake_minimum_required(VERSION 3.20)
project(SurvivalGame VERSION 0.2.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SG_BUILD_CLIENT "Build the OpenGL game client" ON)
option(SG_BUILD_SERVER "Build the dedicated server" ON)
option(SG_BUILD_TESTS  "Build unit tests and the headless bot" ON)
option(SG_PORTABLE     "Linux: link the C++ runtime statically so the binary runs on other distros" OFF)
if(SG_PORTABLE AND UNIX AND NOT APPLE)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# Strict warnings for our code only (not for third-party code like GLFW).
function(sg_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# shared: code used by BOTH client and server so they can never disagree
# (items, recipes, building rules, movement math, terrain, packet format).
# ---------------------------------------------------------------------------
add_library(sg_shared STATIC
  shared/bytes.cpp
  shared/items.cpp
  shared/world.cpp
  shared/net.cpp
  shared/protocol.cpp
  shared/reliable.cpp
  shared/movement.cpp
  shared/terrain.cpp
  shared/net_client.cpp
  shared/upnp.cpp
  shared/adpcm.cpp
)
target_include_directories(sg_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shared)
sg_warnings(sg_shared)
if(WIN32)
  target_link_libraries(sg_shared PUBLIC ws2_32)
endif()

# The server code is also linked into the client (solo / self-hosted servers)
# and the tests, so build it once as a library.
add_library(sg_server_lib STATIC server/server.cpp server/commands.cpp server/monuments.cpp)
target_include_directories(sg_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(sg_server_lib PUBLIC sg_shared)
sg_warnings(sg_server_lib)

# ---------------------------------------------------------------------------
# dedicated server (headless, no graphics dependencies)
# ---------------------------------------------------------------------------
if(SG_BUILD_SERVER)
  add_executable(sg_server server/main.cpp)
  target_link_libraries(sg_server PRIVATE sg_server_lib)
  sg_warnings(sg_server)
endif()

# ---------------------------------------------------------------------------
# client: our own renderer on top of GLFW (window + input), Dear ImGui for
# menus, HUD and inventory. Both are downloaded automatically at configure time.
# ---------------------------------------------------------------------------
if(SG_BUILD_CLIENT)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 also runs on Wayland desktops via XWayland
  endif()
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)
  # miniaudio: single-header audio library (sound effects, 3D positioning). SOURCE_SUBDIR points at
  # nothing so only the header is fetched, not miniaudio's own examples and tests.
  FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-build)
  FetchContent_MakeAvailable(glfw imgui miniaudio)

  add_library(miniaudio_impl STATIC client/miniaudio_impl.c)
  target_include_directories(miniaudio_impl PUBLIC ${miniaudio_SOURCE_DIR})
  if(UNIX AND NOT APPLE)
    target_link_libraries(miniaudio_impl PUBLIC ${CMAKE_DL_LIBS} m)
  endif()

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw)

  add_executable(sg_client
    client/main.cpp
    client/gl.cpp
    client/renderer.cpp
    client/meshes.cpp
    client/character.cpp
    client/ui.cpp
    client/sfx.cpp
    client/audio.cpp
    client/voice.cpp
  )
  target_link_libraries(sg_client PRIVATE sg_server_lib glfw imgui miniaudio_impl)
  target_compile_definitions(sg_client PRIVATE GLFW_INCLUDE_NONE)
  target_include_directories(sg_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/client)
  sg_warnings(sg_client)
  find_package(Threads REQUIRED)
  target_link_libraries(sg_client PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(SG_BUILD_TESTS)
  enable_testing()
  add_executable(sg_tests tests/tests.cpp)
  target_link_libraries(sg_tests PRIVATE sg_server_lib)
  sg_warnings(sg_tests)
  add_test(NAME unit COMMAND sg_tests)

  # Headless bot: connects to a server and walks around. Used for load tests.
  add_executable(sg_bot tests/bot.cpp)
  target_link_libraries(sg_bot PRIVATE sg_shared)
  sg_warnings(sg_bot)
endif()
