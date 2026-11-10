cmake_minimum_required(VERSION 3.20)
project(SurvivalGame VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SG_BUILD_CLIENT "Build the OpenGL game client" ON)
option(SG_BUILD_SERVER "Build the dedicated server" ON)
option(SG_BUILD_TESTS  "Build unit tests and the headless bot" ON)

# Strict warnings for our code only (not for third-party code like GLFW).
function(sg_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# shared: code used by BOTH client and server so they can never disagree
# (movement math, terrain height, packet format, sockets).
# ---------------------------------------------------------------------------
add_library(sg_shared STATIC
  shared/net.cpp
  shared/protocol.cpp
  shared/movement.cpp
  shared/terrain.cpp
  shared/net_client.cpp
)
target_include_directories(sg_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shared)
sg_warnings(sg_shared)
if(WIN32)
  target_link_libraries(sg_shared PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# dedicated server (headless, no graphics dependencies)
# ---------------------------------------------------------------------------
if(SG_BUILD_SERVER)
  add_executable(sg_server server/main.cpp server/server.cpp)
  target_link_libraries(sg_server PRIVATE sg_shared)
  sg_warnings(sg_server)
endif()

# ---------------------------------------------------------------------------
# client: our own renderer on top of GLFW (window + input only).
# GLFW is downloaded automatically at configure time.
# ---------------------------------------------------------------------------
if(SG_BUILD_CLIENT)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)

  add_executable(sg_client
    client/main.cpp
    client/gl.cpp
    client/renderer.cpp
    server/server.cpp
  )
  target_link_libraries(sg_client PRIVATE sg_shared glfw)
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
  add_executable(sg_tests tests/tests.cpp server/server.cpp)
  target_link_libraries(sg_tests PRIVATE sg_shared)
  sg_warnings(sg_tests)
  add_test(NAME unit COMMAND sg_tests)

  # Headless bot: connects to a server and walks around. Used for load tests.
  add_executable(sg_bot tests/bot.cpp)
  target_link_libraries(sg_bot PRIVATE sg_shared)
  sg_warnings(sg_bot)
endif()
